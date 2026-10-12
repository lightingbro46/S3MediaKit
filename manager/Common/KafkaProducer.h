#ifndef COMMON_KAFKAPRODUCER_H
#define COMMON_KAFKAPRODUCER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include "Common/EventRecord.h"

namespace managerkit {

// Connection and tuning of the event bus producer. Credentials stay in memory only.
struct KafkaSettings {
    std::string brokers;                      // host:port,...
    std::string topic;
    std::string securityProtocol = "plaintext"; // plaintext, ssl, sasl_plaintext, sasl_ssl
    std::string saslMechanism;                // PLAIN, SCRAM-SHA-256, SCRAM-SHA-512
    std::string saslUsername;
    std::string saslPassword;
    std::string caPem;                        // CA for TLS; empty = system store
    std::string clientId;
    std::string compression = "lz4";
    int lingerMs = 50;
    int queueMaxKBytes = 16384;
    int messageTimeoutMs = 60000;
    int statsIntervalMs = 1000;               // broker state refresh (statistics callback)
    std::map<std::string, std::string> extra; // raw librdkafka properties set last (tests: test.mock.*)

    bool operator==(const KafkaSettings &o) const;
    bool operator!=(const KafkaSettings &o) const { return !(*this == o); }
};

/**
 * Idempotent librdkafka producer (acks=all, no topic auto-creation) without any blocking call but
 * flush() and the destructor. Callbacks run inside poll()/flush(), on the thread calling them.
 * Only this class includes librdkafka; the build without ENABLE_KAFKA has no implementation.
 */
class KafkaProducer {
public:
    using Ptr = std::shared_ptr<KafkaProducer>;

    // Why records cannot be delivered, for alerts.
    enum class ErrorKind { None, BrokerDown, Auth, Topic, QueueFull, Fatal, Purged, Other };

    // `id` as given to produce(); `error` 0 = delivered. On error `record` is the record rebuilt from
    // the message (without the "replayed" header) so it can be kept elsewhere; empty on success.
    using DeliveryCallback = std::function<void(uint64_t id, int error, ErrorKind kind, const std::string &reason,
                                                const EventRecord &record)>;
    // Errors not tied to one record (all brokers down, authentication, fatal idempotence error).
    using ErrorCallback = std::function<void(int error, ErrorKind kind, bool fatal, const std::string &reason)>;

    enum class ProduceResult { Ok, QueueFull, Error };

    // nullptr with `err` set when the settings are rejected.
    static Ptr create(const KafkaSettings &settings, std::string &err);
    // Blocks until librdkafka stops its threads: flush() first.
    ~KafkaProducer();

    void setDeliveryCallback(DeliveryCallback cb) { _on_delivery = std::move(cb); }
    void setErrorCallback(ErrorCallback cb) { _on_error = std::move(cb); }

    // Never blocks. Adds a "replayed: 1" header when `replayed`.
    ProduceResult produce(const EventRecord &record, uint64_t id, bool replayed, std::string *err = nullptr);
    // Serves callbacks; returns how many events were served.
    int poll(int timeout_ms = 0);
    // Records not yet sent come back through the delivery callback (ErrorKind::Purged) at the next poll.
    void purgeQueued();
    // Waits up to `timeout_ms` for every record to be delivered; returns how many are still queued.
    int flush(int timeout_ms);
    // Records produced and not reported yet.
    int queued() const;

    // Brokers in state UP according to the last statistics; -1 before the first statistics.
    int brokersUp() const { return _brokers_up.load(); }
    int brokersKnown() const { return _brokers_known.load(); }
    // Fatal error raised (idempotence broken): the instance must be replaced.
    bool fatal() const;

    const KafkaSettings &settings() const { return _settings; }

    static ErrorKind classify(int error);
    static const char *kindName(ErrorKind kind);
    static std::string version();

private:
    explicit KafkaProducer(const KafkaSettings &settings);
    // librdkafka C callbacks (KafkaProducer.cpp), given this object as the handle opaque.
    friend struct KafkaCallbacks;

private:
    KafkaSettings _settings;
    void *_rk = nullptr; // rd_kafka_t
    DeliveryCallback _on_delivery;
    ErrorCallback _on_error;
    std::atomic<int> _brokers_up { -1 };
    std::atomic<int> _brokers_known { 0 };
};

} // namespace managerkit

#endif // COMMON_KAFKAPRODUCER_H
