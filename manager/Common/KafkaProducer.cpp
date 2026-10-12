#if defined(ENABLE_KAFKA)

#include <string.h>
#include <librdkafka/rdkafka.h>
#include <json/json.h>
#include "Util/logger.h"
#include "Common/KafkaProducer.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

#define RK static_cast<rd_kafka_t *>(_rk)

bool KafkaSettings::operator==(const KafkaSettings &o) const {
    return brokers == o.brokers && topic == o.topic && securityProtocol == o.securityProtocol &&
           saslMechanism == o.saslMechanism && saslUsername == o.saslUsername && saslPassword == o.saslPassword &&
           caPem == o.caPem && clientId == o.clientId && compression == o.compression && lingerMs == o.lingerMs &&
           queueMaxKBytes == o.queueMaxKBytes && messageTimeoutMs == o.messageTimeoutMs &&
           statsIntervalMs == o.statsIntervalMs && extra == o.extra;
}

struct KafkaCallbacks {
    static void onDelivery(rd_kafka_t *, const rd_kafka_message_t *msg, void *opaque) {
        auto self = static_cast<KafkaProducer *>(opaque);
        if (!self->_on_delivery) {
            return;
        }
        const auto id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(msg->_private));
        EventRecord record;
        string reason;
        if (msg->err) {
            reason = rd_kafka_err2str(msg->err);
            if (msg->key) {
                record.key.assign(static_cast<const char *>(msg->key), msg->key_len);
            }
            if (msg->payload) {
                record.value.assign(static_cast<const char *>(msg->payload), msg->len);
            }
            rd_kafka_headers_t *hdrs = nullptr;
            if (rd_kafka_message_headers(msg, &hdrs) == RD_KAFKA_RESP_ERR_NO_ERROR) {
                const char *name;
                const void *value;
                size_t size;
                for (size_t i = 0; rd_kafka_header_get_all(hdrs, i, &name, &value, &size) == RD_KAFKA_RESP_ERR_NO_ERROR; ++i) {
                    if (strcmp(name, "replayed") != 0) {
                        record.headers.emplace_back(name, string(static_cast<const char *>(value), size));
                    }
                }
            }
        }
        self->_on_delivery(id, msg->err, KafkaProducer::classify(msg->err), reason, record);
    }

    static void onError(rd_kafka_t *rk, int err, const char *reason, void *opaque) {
        auto self = static_cast<KafkaProducer *>(opaque);
        bool fatal = err == RD_KAFKA_RESP_ERR__FATAL;
        string text = reason ? reason : "";
        if (fatal) {
            char buf[512];
            err = rd_kafka_fatal_error(rk, buf, sizeof(buf));
            text = buf;
        }
        WarnL << "Kafka " << (fatal ? "fatal " : "") << "error " << rd_kafka_err2name(static_cast<rd_kafka_resp_err_t>(err))
              << ": " << text;
        if (self->_on_error) {
            self->_on_error(err, fatal ? KafkaProducer::ErrorKind::Fatal : KafkaProducer::classify(err), fatal, text);
        }
    }

    static int onStats(rd_kafka_t *, char *json, size_t json_len, void *opaque) {
        auto self = static_cast<KafkaProducer *>(opaque);
        Json::CharReaderBuilder builder;
        unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value root;
        if (!reader->parse(json, json + json_len, &root, nullptr) || !root.isObject()) {
            return 0;
        }
        int known = 0, up = 0;
        const Json::Value &brokers = root["brokers"];
        if (brokers.isObject()) {
            for (auto it = brokers.begin(); it != brokers.end(); ++it) {
                // Bootstrap entries have nodeid -1, the coordinator handles are "logical".
                if ((*it)["nodeid"].asInt() < 0 || (*it)["source"].asString() == "logical") {
                    continue;
                }
                ++known;
                if ((*it)["state"].asString() == "UP") {
                    ++up;
                }
            }
        }
        self->_brokers_known = known;
        self->_brokers_up = up;
        return 0; // librdkafka frees json
    }

    static void onLog(const rd_kafka_t *, int level, const char *fac, const char *buf) {
        // syslog levels; may run on librdkafka threads (the logger is thread-safe).
        if (level <= 4) {
            WarnL << "Kafka " << fac << ": " << buf;
        } else {
            DebugL << "Kafka " << fac << ": " << buf;
        }
    }
};

string KafkaProducer::version() {
    return rd_kafka_version_str();
}

KafkaProducer::ErrorKind KafkaProducer::classify(int error) {
    switch (error) {
        case RD_KAFKA_RESP_ERR_NO_ERROR: return ErrorKind::None;
        case RD_KAFKA_RESP_ERR__TRANSPORT:
        case RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN:
        case RD_KAFKA_RESP_ERR__RESOLVE:
        case RD_KAFKA_RESP_ERR__MSG_TIMED_OUT:
        case RD_KAFKA_RESP_ERR__TIMED_OUT:
        case RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT:
        case RD_KAFKA_RESP_ERR_NETWORK_EXCEPTION:
        case RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION:
        case RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS:
        case RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND: return ErrorKind::BrokerDown;
        case RD_KAFKA_RESP_ERR__AUTHENTICATION:
        case RD_KAFKA_RESP_ERR__SSL:
        case RD_KAFKA_RESP_ERR_SASL_AUTHENTICATION_FAILED:
        case RD_KAFKA_RESP_ERR_UNSUPPORTED_SASL_MECHANISM:
        case RD_KAFKA_RESP_ERR_ILLEGAL_SASL_STATE:
        case RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED: return ErrorKind::Auth;
        case RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC:
        case RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART:
        case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED: return ErrorKind::Topic;
        case RD_KAFKA_RESP_ERR__QUEUE_FULL: return ErrorKind::QueueFull;
        case RD_KAFKA_RESP_ERR__FATAL: return ErrorKind::Fatal;
        case RD_KAFKA_RESP_ERR__PURGE_QUEUE:
        case RD_KAFKA_RESP_ERR__PURGE_INFLIGHT: return ErrorKind::Purged;
        default: return ErrorKind::Other;
    }
}

const char *KafkaProducer::kindName(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::None: return "none";
        case ErrorKind::BrokerDown: return "broker_down";
        case ErrorKind::Auth: return "auth";
        case ErrorKind::Topic: return "topic";
        case ErrorKind::QueueFull: return "queue_full";
        case ErrorKind::Fatal: return "fatal";
        case ErrorKind::Purged: return "purged";
        default: return "other";
    }
}

KafkaProducer::KafkaProducer(const KafkaSettings &settings) : _settings(settings) {}

KafkaProducer::Ptr KafkaProducer::create(const KafkaSettings &settings, string &err) {
    if (settings.brokers.empty() || settings.topic.empty()) {
        err = "brokers and topic are required";
        return nullptr;
    }
    Ptr ret(new KafkaProducer(settings));
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    char errstr[512];
    auto set = [&](const string &key, const string &value) {
        if (rd_kafka_conf_set(conf, key.data(), value.data(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
            // Never echo the value: it may be a password.
            err = key + ": " + errstr;
            return false;
        }
        return true;
    };
    bool ok = set("bootstrap.servers", settings.brokers) && set("enable.idempotence", "true") && set("acks", "all") &&
              set("allow.auto.create.topics", "false") &&
              // Keep a connection to every broker: while events are spooled nothing is produced, and a
              // sparse client would never reconnect, so brokersUp() would stay 0 after the outage.
              set("enable.sparse.connections", "false") && set("compression.codec", settings.compression) &&
              set("linger.ms", to_string(settings.lingerMs)) &&
              set("queue.buffering.max.kbytes", to_string(settings.queueMaxKBytes)) &&
              set("message.timeout.ms", to_string(settings.messageTimeoutMs)) &&
              set("statistics.interval.ms", to_string(settings.statsIntervalMs)) &&
              set("security.protocol", settings.securityProtocol.empty() ? "plaintext" : settings.securityProtocol);
    if (ok && !settings.clientId.empty()) {
        ok = set("client.id", settings.clientId);
    }
    if (ok && !settings.saslMechanism.empty()) {
        ok = set("sasl.mechanism", settings.saslMechanism) && set("sasl.username", settings.saslUsername) &&
             set("sasl.password", settings.saslPassword);
    }
    if (ok && !settings.caPem.empty()) {
        ok = set("ssl.ca.pem", settings.caPem);
    }
    for (auto &kv : settings.extra) {
        if (!ok) {
            break;
        }
        ok = set(kv.first, kv.second);
    }
    if (!ok) {
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    rd_kafka_conf_set_opaque(conf, ret.get());
    rd_kafka_conf_set_dr_msg_cb(conf, KafkaCallbacks::onDelivery);
    rd_kafka_conf_set_error_cb(conf, KafkaCallbacks::onError);
    rd_kafka_conf_set_stats_cb(conf, KafkaCallbacks::onStats);
    rd_kafka_conf_set_log_cb(conf, KafkaCallbacks::onLog);

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk) {
        err = errstr;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    ret->_rk = rk; // owns conf now
    return ret;
}

KafkaProducer::~KafkaProducer() {
    if (_rk) {
        rd_kafka_destroy(RK);
    }
}

KafkaProducer::ProduceResult KafkaProducer::produce(const EventRecord &record, uint64_t id, bool replayed, string *err) {
    rd_kafka_headers_t *headers = rd_kafka_headers_new(record.headers.size() + 1);
    for (auto &h : record.headers) {
        rd_kafka_header_add(headers, h.first.data(), static_cast<ssize_t>(h.first.size()), h.second.data(),
                            static_cast<ssize_t>(h.second.size()));
    }
    if (replayed) {
        rd_kafka_header_add(headers, "replayed", -1, "1", 1);
    }
    // No RD_KAFKA_MSG_F_BLOCK: a full queue is reported, never waited on.
    rd_kafka_resp_err_t rc = rd_kafka_producev(
        RK, RD_KAFKA_V_TOPIC(_settings.topic.data()), RD_KAFKA_V_KEY(record.key.data(), record.key.size()),
        RD_KAFKA_V_VALUE(const_cast<char *>(record.value.data()), record.value.size()),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY), RD_KAFKA_V_HEADERS(headers),
        RD_KAFKA_V_OPAQUE(reinterpret_cast<void *>(static_cast<uintptr_t>(id))), RD_KAFKA_V_END);
    if (rc == RD_KAFKA_RESP_ERR_NO_ERROR) {
        return ProduceResult::Ok; // headers owned by the message
    }
    rd_kafka_headers_destroy(headers);
    if (err) {
        *err = rd_kafka_err2str(rc);
    }
    return rc == RD_KAFKA_RESP_ERR__QUEUE_FULL ? ProduceResult::QueueFull : ProduceResult::Error;
}

int KafkaProducer::poll(int timeout_ms) {
    return rd_kafka_poll(RK, timeout_ms);
}

void KafkaProducer::purgeQueued() {
    rd_kafka_purge(RK, RD_KAFKA_PURGE_F_QUEUE);
}

int KafkaProducer::flush(int timeout_ms) {
    rd_kafka_flush(RK, timeout_ms);
    return rd_kafka_outq_len(RK);
}

int KafkaProducer::queued() const {
    return rd_kafka_outq_len(RK);
}

bool KafkaProducer::fatal() const {
    return rd_kafka_fatal_error(RK, nullptr, 0) != RD_KAFKA_RESP_ERR_NO_ERROR;
}

} // namespace managerkit

#endif // ENABLE_KAFKA
