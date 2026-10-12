// KafkaProducer against the in-process librdkafka mock cluster.

#include <gtest/gtest.h>
#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>
#include <chrono>
#include <functional>
#include <map>
#include <thread>
#include <vector>
#include "Common/KafkaProducer.h"

using namespace managerkit;

namespace {

bool pollUntil(KafkaProducer &producer, const std::function<bool()> &pred, int timeout_ms = 8000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        producer.poll(50);
        if (pred()) return true;
    }
    return pred();
}

// A mock cluster owned by a bare producer handle (only used to keep the cluster alive).
class MockCluster {
public:
    explicit MockCluster(int brokers) {
        char errstr[256];
        rd_kafka_conf_t *conf = rd_kafka_conf_new();
        rd_kafka_conf_set(conf, "log_level", "0", errstr, sizeof(errstr));
        _owner = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
        _mc = rd_kafka_mock_cluster_new(_owner, brokers);
        _brokers = brokers;
    }
    ~MockCluster() {
        rd_kafka_mock_cluster_destroy(_mc);
        rd_kafka_destroy(_owner);
    }
    std::string bootstraps() const { return rd_kafka_mock_cluster_bootstraps(_mc); }
    void createTopic(const std::string &name, int partitions) {
        ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_mock_topic_create(_mc, name.c_str(), partitions, 1));
    }
    void setAll(bool up) {
        for (int id = 1; id <= _brokers; ++id) {
            if (up) {
                rd_kafka_mock_broker_set_up(_mc, id);
            } else {
                rd_kafka_mock_broker_set_down(_mc, id);
            }
        }
    }

private:
    rd_kafka_t *_owner = nullptr;
    rd_kafka_mock_cluster_t *_mc = nullptr;
    int _brokers = 0;
};

KafkaSettings settingsFor(const MockCluster &mc, const std::string &topic) {
    KafkaSettings s;
    s.brokers = mc.bootstraps();
    s.topic = topic;
    s.clientId = "unit-test";
    s.lingerMs = 5;
    s.messageTimeoutMs = 5000;
    s.extra["log_level"] = "3";
    return s;
}

EventRecord makeRecord(const std::string &key, int seq) {
    EventRecord r;
    r.key = key;
    r.headers = { { "schema", "device-event" }, { "seq", std::to_string(seq) } };
    r.value = "{\"seq\":" + std::to_string(seq) + "}";
    return r;
}

struct Delivery {
    uint64_t id;
    int error;
    KafkaProducer::ErrorKind kind;
};

} // namespace

TEST(KafkaProducerTest, RejectsIncompleteOrInvalidSettings) {
    std::string err;
    KafkaSettings s;
    s.topic = "t";
    EXPECT_FALSE(KafkaProducer::create(s, err));
    EXPECT_FALSE(err.empty());

    s.brokers = "127.0.0.1:1";
    s.securityProtocol = "carrier_pigeon";
    err.clear();
    EXPECT_FALSE(KafkaProducer::create(s, err));
    EXPECT_NE(std::string::npos, err.find("security.protocol")) << err;

    s.securityProtocol = "sasl_ssl";
    s.saslMechanism = "SCRAM-SHA-512";
    s.saslUsername = "ms-01";
    s.saslPassword = "s3cr3t-value";
    s.caPem = "not a certificate";
    err.clear();
    auto p = KafkaProducer::create(s, err);
    // A bad CA is rejected at creation; the password never appears in the error text.
    EXPECT_FALSE(p);
    EXPECT_EQ(std::string::npos, err.find("s3cr3t-value")) << err;

    s.caPem.clear();
    err.clear();
    EXPECT_TRUE(KafkaProducer::create(s, err)) << err;
}

TEST(KafkaProducerTest, DeliversEveryRecordInOrderPerKey) {
    MockCluster mc(3);
    mc.createTopic("device-events", 3);
    std::string err;
    auto producer = KafkaProducer::create(settingsFor(mc, "device-events"), err);
    ASSERT_TRUE(producer) << err;

    std::vector<Delivery> deliveries;
    producer->setDeliveryCallback([&](uint64_t id, int error, KafkaProducer::ErrorKind kind, const std::string &,
                                       const EventRecord &) {
        deliveries.push_back({ id, error, kind });
    });
    const int total = 300;
    for (int i = 0; i < total; ++i) {
        ASSERT_EQ(KafkaProducer::ProduceResult::Ok,
                  producer->produce(makeRecord("cam-" + std::to_string(i % 5), i), static_cast<uint64_t>(i), false));
    }
    ASSERT_TRUE(pollUntil(*producer, [&]() { return deliveries.size() == static_cast<size_t>(total); }));
    std::map<int, uint64_t> last_per_key;
    for (auto &d : deliveries) {
        EXPECT_EQ(0, d.error);
        EXPECT_EQ(KafkaProducer::ErrorKind::None, d.kind);
        int key = static_cast<int>(d.id % 5);
        auto it = last_per_key.find(key);
        if (it != last_per_key.end()) {
            EXPECT_LT(it->second, d.id) << "key " << key;
        }
        last_per_key[key] = d.id;
    }
    EXPECT_TRUE(pollUntil(*producer, [&]() { return producer->brokersUp() == 3; }, 5000)) << producer->brokersUp();
    EXPECT_EQ(3, producer->brokersKnown());
    EXPECT_EQ(0, producer->queued());
    EXPECT_FALSE(producer->fatal());
}

TEST(KafkaProducerTest, HeadersKeyAndValueReachTheBroker) {
    MockCluster mc(1);
    mc.createTopic("device-events", 1);
    std::string err;
    auto producer = KafkaProducer::create(settingsFor(mc, "device-events"), err);
    ASSERT_TRUE(producer) << err;
    int delivered = 0;
    producer->setDeliveryCallback([&](uint64_t, int error, KafkaProducer::ErrorKind, const std::string &,
                                       const EventRecord &) {
        EXPECT_EQ(0, error);
        ++delivered;
    });
    ASSERT_EQ(KafkaProducer::ProduceResult::Ok, producer->produce(makeRecord("cam-1", 7), 1, true));
    ASSERT_EQ(0, producer->flush(5000));
    ASSERT_EQ(1, delivered);

    char errstr[256];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "bootstrap.servers", mc.bootstraps().c_str(), errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "group.id", "unit", errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "log_level", "3", errstr, sizeof(errstr));
    rd_kafka_t *consumer = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    ASSERT_TRUE(consumer) << errstr;
    rd_kafka_topic_partition_list_t *parts = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(parts, "device-events", 0)->offset = RD_KAFKA_OFFSET_BEGINNING;
    ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_assign(consumer, parts));
    rd_kafka_topic_partition_list_destroy(parts);

    rd_kafka_message_t *msg = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!msg && std::chrono::steady_clock::now() < deadline) {
        msg = rd_kafka_consumer_poll(consumer, 100);
        if (msg && msg->err) {
            rd_kafka_message_destroy(msg);
            msg = nullptr;
        }
    }
    ASSERT_TRUE(msg);
    EXPECT_EQ("cam-1", std::string(static_cast<const char *>(msg->key), msg->key_len));
    EXPECT_EQ("{\"seq\":7}", std::string(static_cast<const char *>(msg->payload), msg->len));
    rd_kafka_headers_t *hdrs = nullptr;
    ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_message_headers(msg, &hdrs));
    std::map<std::string, std::string> got;
    const char *name;
    const void *value;
    size_t size;
    for (size_t i = 0; rd_kafka_header_get_all(hdrs, i, &name, &value, &size) == RD_KAFKA_RESP_ERR_NO_ERROR; ++i) {
        got[name] = std::string(static_cast<const char *>(value), size);
    }
    EXPECT_EQ("device-event", got["schema"]);
    EXPECT_EQ("7", got["seq"]);
    EXPECT_EQ("1", got["replayed"]);
    rd_kafka_message_destroy(msg);
    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);
}

TEST(KafkaProducerTest, FullQueueIsReportedWithoutBlocking) {
    MockCluster mc(1);
    mc.createTopic("device-events", 1);
    mc.setAll(false);
    auto s = settingsFor(mc, "device-events");
    s.extra["queue.buffering.max.messages"] = "5";
    std::string err;
    auto producer = KafkaProducer::create(s, err);
    ASSERT_TRUE(producer) << err;

    int ok = 0, full = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        auto rc = producer->produce(makeRecord("cam-1", i), static_cast<uint64_t>(i), false);
        if (rc == KafkaProducer::ProduceResult::Ok) ++ok;
        if (rc == KafkaProducer::ProduceResult::QueueFull) ++full;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(5, ok);
    EXPECT_EQ(15, full);
    EXPECT_LT(ms, 200);
    EXPECT_EQ(KafkaProducer::ErrorKind::QueueFull, KafkaProducer::classify(RD_KAFKA_RESP_ERR__QUEUE_FULL));
}

TEST(KafkaProducerTest, PurgeHandsQueuedRecordsBack) {
    MockCluster mc(1);
    mc.createTopic("device-events", 1);
    mc.setAll(false);
    std::string err;
    auto producer = KafkaProducer::create(settingsFor(mc, "device-events"), err);
    ASSERT_TRUE(producer) << err;
    std::vector<Delivery> deliveries;
    std::vector<EventRecord> records;
    producer->setDeliveryCallback([&](uint64_t id, int error, KafkaProducer::ErrorKind kind, const std::string &,
                                       const EventRecord &record) {
        deliveries.push_back({ id, error, kind });
        records.push_back(record);
    });
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(KafkaProducer::ProduceResult::Ok, producer->produce(makeRecord("cam-1", i), 100 + i, false));
    }
    producer->purgeQueued();
    ASSERT_TRUE(pollUntil(*producer, [&]() { return deliveries.size() == 10; }, 3000)) << deliveries.size();
    for (size_t i = 0; i < deliveries.size(); ++i) {
        EXPECT_EQ(100u + i, deliveries[i].id);
        EXPECT_NE(0, deliveries[i].error);
        EXPECT_EQ(KafkaProducer::ErrorKind::Purged, deliveries[i].kind);
    }
    ASSERT_EQ(10u, records.size());
    EXPECT_EQ("cam-1", records[3].key);
    EXPECT_EQ("{\"seq\":3}", records[3].value);
    EXPECT_EQ(makeRecord("cam-1", 3).headers, records[3].headers);
    EXPECT_EQ(0, producer->queued());
}

TEST(KafkaProducerTest, BrokerDownTimesOutThenRecovers) {
    MockCluster mc(1);
    mc.createTopic("device-events", 1);
    auto s = settingsFor(mc, "device-events");
    s.messageTimeoutMs = 1500;
    // Reconnect well within the message timeout once the broker is back (default backoff up to 10 s).
    s.extra["reconnect.backoff.ms"] = "50";
    s.extra["reconnect.backoff.max.ms"] = "200";
    std::string err;
    auto producer = KafkaProducer::create(s, err);
    ASSERT_TRUE(producer) << err;
    std::vector<Delivery> deliveries;
    producer->setDeliveryCallback([&](uint64_t id, int error, KafkaProducer::ErrorKind kind, const std::string &,
                                       const EventRecord &) {
        deliveries.push_back({ id, error, kind });
    });
    ASSERT_EQ(KafkaProducer::ProduceResult::Ok, producer->produce(makeRecord("cam-1", 0), 0, false));
    ASSERT_TRUE(pollUntil(*producer, [&]() { return deliveries.size() == 1; }));
    EXPECT_EQ(0, deliveries[0].error);
    ASSERT_TRUE(pollUntil(*producer, [&]() { return producer->brokersUp() == 1; }, 4000));

    mc.setAll(false);
    ASSERT_TRUE(pollUntil(*producer, [&]() { return producer->brokersUp() == 0; }, 5000)) << producer->brokersUp();
    ASSERT_EQ(KafkaProducer::ProduceResult::Ok, producer->produce(makeRecord("cam-1", 1), 1, false));
    ASSERT_TRUE(pollUntil(*producer, [&]() { return deliveries.size() == 2; }, 5000));
    EXPECT_NE(0, deliveries[1].error);
    EXPECT_EQ(KafkaProducer::ErrorKind::BrokerDown, deliveries[1].kind);

    mc.setAll(true);
    ASSERT_EQ(KafkaProducer::ProduceResult::Ok, producer->produce(makeRecord("cam-1", 2), 2, false));
    ASSERT_TRUE(pollUntil(*producer, [&]() { return deliveries.size() == 3; }, 8000));
    EXPECT_EQ(0, deliveries[2].error);
    EXPECT_TRUE(pollUntil(*producer, [&]() { return producer->brokersUp() == 1; }, 4000));
}
