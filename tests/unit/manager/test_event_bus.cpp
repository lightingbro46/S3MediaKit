// EventBus (LIVE / SPOOL / DRAIN) and the OnvifEventHub publisher against the librdkafka mock cluster.

#include <gtest/gtest.h>
#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>
#include <stdlib.h>
#include <chrono>
#include <functional>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "Util/NoticeCenter.h"
#include "Util/mini.h"
#include <json/json.h>
#include "Common/config.h"
#include "Common/EventBus.h"
#include "Control/OnvifDeviceControl.h"
#include "Control/OnvifEventHub.h"
#include "Control/OnvifEventSubscriber.h"
#include "Onvif/OnvifEventTopics.h"

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

namespace {

bool waitFor(const std::function<bool()> &pred, int timeout_ms = 10000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

class MockCluster {
public:
    explicit MockCluster(int brokers) : _brokers(brokers) {
        char errstr[256];
        rd_kafka_conf_t *conf = rd_kafka_conf_new();
        rd_kafka_conf_set(conf, "log_level", "0", errstr, sizeof(errstr));
        _owner = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
        _mc = rd_kafka_mock_cluster_new(_owner, brokers);
    }
    ~MockCluster() {
        rd_kafka_mock_cluster_destroy(_mc);
        rd_kafka_destroy(_owner);
    }
    std::string bootstraps() const { return rd_kafka_mock_cluster_bootstraps(_mc); }
    void createTopic(const std::string &name, int partitions) {
        rd_kafka_mock_topic_create(_mc, name.c_str(), partitions, 1);
    }
    void setAll(bool up) {
        for (int id = 1; id <= _brokers; ++id) {
            up ? rd_kafka_mock_broker_set_up(_mc, id) : rd_kafka_mock_broker_set_down(_mc, id);
        }
    }

private:
    int _brokers;
    rd_kafka_t *_owner = nullptr;
    rd_kafka_mock_cluster_t *_mc = nullptr;
};

struct Consumed {
    std::string key;
    std::string value;
    std::map<std::string, std::string> headers;
};

// Everything in the topic, partition by partition (order kept within a partition = within a key).
std::vector<Consumed> consumeAll(const MockCluster &mc, const std::string &topic, int partitions, size_t expect_at_least) {
    char errstr[256];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "bootstrap.servers", mc.bootstraps().c_str(), errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "group.id", "unit", errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "log_level", "0", errstr, sizeof(errstr));
    rd_kafka_t *consumer = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    rd_kafka_topic_partition_list_t *parts = rd_kafka_topic_partition_list_new(partitions);
    for (int p = 0; p < partitions; ++p) {
        rd_kafka_topic_partition_list_add(parts, topic.c_str(), p)->offset = RD_KAFKA_OFFSET_BEGINNING;
    }
    rd_kafka_assign(consumer, parts);
    rd_kafka_topic_partition_list_destroy(parts);

    std::vector<Consumed> ret;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto idle_since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        rd_kafka_message_t *msg = rd_kafka_consumer_poll(consumer, 100);
        if (!msg) {
            if (ret.size() >= expect_at_least &&
                std::chrono::steady_clock::now() - idle_since > std::chrono::milliseconds(700)) {
                break;
            }
            continue;
        }
        if (!msg->err) {
            Consumed c;
            c.key.assign(static_cast<const char *>(msg->key), msg->key_len);
            c.value.assign(static_cast<const char *>(msg->payload), msg->len);
            rd_kafka_headers_t *hdrs = nullptr;
            if (rd_kafka_message_headers(msg, &hdrs) == RD_KAFKA_RESP_ERR_NO_ERROR) {
                const char *name;
                const void *value;
                size_t size;
                for (size_t i = 0; rd_kafka_header_get_all(hdrs, i, &name, &value, &size) == RD_KAFKA_RESP_ERR_NO_ERROR; ++i) {
                    c.headers[name] = std::string(static_cast<const char *>(value), size);
                }
            }
            ret.push_back(std::move(c));
            idle_since = std::chrono::steady_clock::now();
        }
        rd_kafka_message_destroy(msg);
    }
    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);
    return ret;
}

EventRecord makeRecord(int seq) {
    EventRecord r;
    r.key = "cam-" + std::to_string(seq % 4);
    r.headers = { { "seq", std::to_string(seq) } };
    r.value = std::to_string(seq);
    return r;
}

struct AlertLog {
    std::mutex mtx;
    std::vector<std::pair<std::string, std::string>> alerts; // code, reason
    size_t count(const std::string &code) {
        std::lock_guard<std::mutex> lck(mtx);
        size_t n = 0;
        for (auto &a : alerts) n += a.first == code;
        return n;
    }
};

class EventBusTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/event_bus_test_XXXXXX";
        ASSERT_NE(nullptr, mkdtemp(tmpl));
        _root = tmpl;
        mINI::Instance()[KafkaCfg::kSpoolDir] = _root + "/spool";
        mINI::Instance()[KafkaCfg::kSpoolAfterSec] = 1;
        mINI::Instance()[KafkaCfg::kSpoolMinFreeMB] = 0;
        mINI::Instance()[KafkaCfg::kFlushTimeoutMs] = 2000;
        mINI::Instance()[KafkaCfg::kMaxEventsPerCameraPerMin] = 0;
        mINI::Instance()[KafkaCfg::kStatsIntervalSec] = 0;
        mINI::Instance()[KafkaCfg::kSpoolFsyncMs] = 200;
        // GET_CONFIG keeps the value read first until a reload.
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
        NoticeCenter::Instance().addListener(&_tag, Broadcast::kBroadcastEventBusAlert, [this](BroadcastEventBusAlertArgs) {
            std::lock_guard<std::mutex> lck(_alerts.mtx);
            _alerts.alerts.emplace_back(event_code, reason);
        });
    }
    void TearDown() override {
        NoticeCenter::Instance().delListener(&_tag, Broadcast::kBroadcastEventBusAlert);
        std::string cmd = "rm -rf '" + _root + "'";
        ASSERT_EQ(0, system(cmd.c_str()));
    }

    static KafkaSettings settingsFor(const MockCluster &mc) {
        KafkaSettings s;
        s.brokers = mc.bootstraps();
        s.topic = "device-events";
        s.lingerMs = 5;
        s.messageTimeoutMs = 3000;
        s.extra["reconnect.backoff.ms"] = "50";
        s.extra["reconnect.backoff.max.ms"] = "200";
        s.extra["log_level"] = "0";
        return s;
    }

    static EventBus::Ptr startBus(const MockCluster &mc) {
        auto bus = EventBus::create(50);
        bus->apply(true, settingsFor(mc));
        bus->waitIdle();
        return bus;
    }

    int _tag = 0;
    std::string _root;
    AlertLog _alerts;
};

std::map<std::string, std::vector<int>> seqsByKey(const std::vector<Consumed> &msgs) {
    std::map<std::string, std::vector<int>> ret;
    for (auto &m : msgs) ret[m.key].push_back(std::stoi(m.value));
    return ret;
}

} // namespace

TEST_F(EventBusTest, OffUntilConfigured) {
    auto bus = EventBus::create(50);
    EXPECT_FALSE(bus->enabled());
    EXPECT_FALSE(bus->publish(makeRecord(0), "cam-0"));
    KafkaSettings incomplete;
    incomplete.brokers = "127.0.0.1:1";
    bus->apply(true, incomplete); // no topic
    bus->waitIdle();
    EXPECT_FALSE(bus->enabled());
    EXPECT_EQ(EventBus::Mode::Off, bus->mode());
    bus->shutdown();
}

TEST_F(EventBusTest, LiveDeliveryAndRateLimit) {
    MockCluster mc(1);
    mc.createTopic("device-events", 2);
    mINI::Instance()[KafkaCfg::kMaxEventsPerCameraPerMin] = 30;
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    auto bus = startBus(mc);
    ASSERT_EQ(EventBus::Mode::Live, bus->mode());
    int accepted = 0;
    for (int i = 0; i < 40; ++i) {
        // One rate key: the 31st event of the minute is refused.
        accepted += bus->publish(makeRecord(i), "cam-x");
    }
    EXPECT_EQ(30, accepted);
    EXPECT_EQ(10u, bus->stats().droppedRate);
    ASSERT_TRUE(waitFor([&]() { return bus->stats().delivered == 30; })) << bus->stats().delivered;
    EXPECT_EQ(0u, bus->stats().spooled);
    bus->shutdown();
    auto msgs = consumeAll(mc, "device-events", 2, 30);
    EXPECT_EQ(30u, msgs.size());
    EXPECT_EQ(0u, _alerts.count("EventBusUnavailable"));
}

TEST_F(EventBusTest, BrokerOutageSpoolsThenReplaysInOrder) {
    MockCluster mc(1);
    mc.createTopic("device-events", 4);
    auto bus = startBus(mc);
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(bus->publish(makeRecord(i), "x"));
    ASSERT_TRUE(waitFor([&]() { return bus->stats().delivered == 10; }));

    mc.setAll(false);
    ASSERT_TRUE(waitFor([&]() { return bus->mode() == EventBus::Mode::Spool; })) << EventBus::modeName(bus->mode());
    for (int i = 10; i < 60; ++i) ASSERT_TRUE(bus->publish(makeRecord(i), "x"));
    ASSERT_TRUE(waitFor([&]() { return bus->stats().spoolPending == 50; })) << bus->stats().spoolPending;
    ASSERT_TRUE(waitFor([&]() { return _alerts.count("EventBusUnavailable") == 1; }));

    mc.setAll(true);
    // Published while draining (or after): must come after the spooled ones of the same key.
    ASSERT_TRUE(waitFor([&]() { return bus->mode() != EventBus::Mode::Spool; }))
        << EventBus::modeName(bus->mode()) << " brokers_up=" << bus->stats().brokersUp << "/" << bus->stats().brokersKnown;
    for (int i = 60; i < 70; ++i) ASSERT_TRUE(bus->publish(makeRecord(i), "x"));
    ASSERT_TRUE(waitFor([&]() { return bus->mode() == EventBus::Mode::Live; })) << EventBus::modeName(bus->mode());
    for (int i = 70; i < 80; ++i) ASSERT_TRUE(bus->publish(makeRecord(i), "x"));
    ASSERT_TRUE(waitFor([&]() { return bus->stats().delivered >= 80; })) << bus->stats().delivered;
    ASSERT_TRUE(waitFor([&]() { return _alerts.count("EventBusRecovered") == 1; }));
    EXPECT_EQ(1u, _alerts.count("EventBusUnavailable"));
    EXPECT_EQ(0u, bus->stats().spoolPending);
    EXPECT_GE(bus->stats().replayed, 50u);
    bus->shutdown();

    auto msgs = consumeAll(mc, "device-events", 4, 80);
    std::set<int> seen;
    for (auto &m : msgs) {
        int seq = std::stoi(m.value);
        seen.insert(seq);
        if (seq >= 10 && seq < 60) {
            EXPECT_EQ("1", m.headers["replayed"]) << seq;
        }
        if (seq >= 70) {
            EXPECT_EQ(0u, m.headers.count("replayed")) << seq;
        }
    }
    EXPECT_EQ(80u, seen.size()); // nothing lost (duplicates allowed)
    for (auto &kv : seqsByKey(msgs)) {
        // In order per key, counting the first copy of each record.
        std::vector<int> firsts;
        std::set<int> dedup;
        for (int seq : kv.second) if (dedup.insert(seq).second) firsts.push_back(seq);
        EXPECT_TRUE(std::is_sorted(firsts.begin(), firsts.end())) << kv.first;
    }
}

TEST_F(EventBusTest, ShutdownWithBrokerDownKeepsEventsForTheNextRun) {
    {
        MockCluster mc(1);
        mc.createTopic("device-events", 1);
        auto bus = startBus(mc);
        mc.setAll(false);
        // Taken live, stuck in librdkafka: shutdown purges them into the spool.
        for (int i = 0; i < 20; ++i) ASSERT_TRUE(bus->publish(makeRecord(i), "x"));
        bus->shutdown();
        EXPECT_EQ(20u, bus->stats().spoolPending);
        EXPECT_EQ(0u, bus->stats().delivered);
    }
    MockCluster mc(1);
    mc.createTopic("device-events", 1);
    auto bus = startBus(mc);
    ASSERT_TRUE(waitFor([&]() { return bus->stats().replayed == 20; })) << bus->stats().replayed;
    ASSERT_TRUE(waitFor([&]() { return bus->mode() == EventBus::Mode::Live; }));
    bus->shutdown();
    auto msgs = consumeAll(mc, "device-events", 1, 20);
    ASSERT_EQ(20u, msgs.size());
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(std::to_string(i), msgs[i].value);
        EXPECT_EQ("1", msgs[i].headers["replayed"]);
    }
}

TEST_F(EventBusTest, SettingsChangeWhileSendingLosesNothing) {
    MockCluster mc(1);
    mc.createTopic("device-events", 2);
    auto bus = startBus(mc);
    std::thread writer([&]() {
        for (int i = 0; i < 200; ++i) {
            bus->publish(makeRecord(i), "x");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto s = settingsFor(mc);
    s.clientId = "after-change";
    bus->apply(true, s);
    writer.join();
    ASSERT_TRUE(waitFor([&]() { return bus->mode() == EventBus::Mode::Live && bus->stats().spoolPending == 0; }));
    bus->shutdown();
    auto msgs = consumeAll(mc, "device-events", 2, 200);
    std::set<int> seen;
    for (auto &m : msgs) seen.insert(std::stoi(m.value));
    EXPECT_EQ(200u, seen.size());
}

TEST_F(EventBusTest, DisabledKeepsTheSpoolAndStopsTaking) {
    MockCluster mc(1);
    mc.createTopic("device-events", 1);
    auto bus = startBus(mc);
    mc.setAll(false);
    ASSERT_TRUE(waitFor([&]() { return bus->mode() == EventBus::Mode::Spool; }));
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(bus->publish(makeRecord(i), "x"));
    ASSERT_TRUE(waitFor([&]() { return bus->stats().spoolPending == 5; }));
    bus->apply(false, KafkaSettings());
    bus->waitIdle();
    EXPECT_EQ(EventBus::Mode::Off, bus->mode());
    EXPECT_FALSE(bus->publish(makeRecord(5), "x"));
    // Enabled again with the broker back: the kept events are sent.
    mc.setAll(true);
    bus->apply(true, settingsFor(mc));
    ASSERT_TRUE(waitFor([&]() { return bus->mode() == EventBus::Mode::Live && bus->stats().replayed == 5; }))
        << EventBus::modeName(bus->mode()) << " " << bus->stats().replayed;
    bus->shutdown();
}

// OnvifEventHub -> EventBus::Instance(): Initialized handling (D3/D7) end to end.
TEST_F(EventBusTest, HubSendsChangesAndResyncsOnly) {
    MockCluster mc(1);
    mc.createTopic("device-events", 1);
    mINI::Instance()[General::kMediaServerId] = "ms-unit";
    auto &bus = EventBus::Instance();
    OnvifEventHub::Instance(); // listens to kBroadcastOnvifEvent

    auto emit = [](const std::string &op, const std::string &value) {
        OnvifEventMessage ev;
        ev.topic = "tns1:RuleEngine/CellMotionDetector/Motion";
        auto rule = OnvifEventTopics::classify(ev.topic);
        ev.eventType = rule->eventType;
        ev.stateItem = rule->stateItem;
        ev.propertyOp = op;
        ev.source = { { "VideoSourceConfigurationToken", "VSC_1" } };
        ev.data = { { "IsMotion", value } };
        std::string device_id = "cam-hub", address = "http://cam";
        int64_t lag_ms = 0;
        NOTICE_EMIT(BroadcastOnvifEventArgs, kBroadcastOnvifEvent, device_id, address, ev, lag_ms);
    };
    // Bus off: nothing sent, nothing remembered.
    emit("Changed", "true");

    bus.apply(true, settingsFor(mc));
    bus.waitIdle();
    ASSERT_TRUE(bus.enabled());
    emit("Initialized", "false"); // nothing sent before: dropped
    emit("Changed", "true");      // sent
    emit("Initialized", "true");  // same as sent: dropped
    emit("Initialized", "false"); // differs: resync
    emit("Deleted", "");          // sent, forgets the state
    emit("Initialized", "true");  // nothing remembered: dropped
    ASSERT_TRUE(waitFor([&]() { return bus.stats().delivered >= 3; })) << bus.stats().delivered;
    EXPECT_EQ(3u, bus.stats().skippedInitialized);
    EXPECT_EQ(1u, bus.stats().resynced);
    bus.apply(false, KafkaSettings());
    bus.waitIdle();

    auto msgs = consumeAll(mc, "device-events", 1, 3);
    ASSERT_EQ(3u, msgs.size());
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::vector<Json::Value> bodies;
    for (auto &m : msgs) {
        Json::Value v;
        ASSERT_TRUE(reader->parse(m.value.data(), m.value.data() + m.value.size(), &v, nullptr));
        EXPECT_EQ("cam-hub", m.key);
        bodies.push_back(v);
    }
    EXPECT_EQ("Changed", bodies[0]["propertyOp"].asString());
    EXPECT_TRUE(bodies[0]["active"].asBool());
    EXPECT_FALSE(bodies[0]["resync"].asBool());
    EXPECT_EQ("ms-unit", bodies[0]["mediaServerId"].asString());
    EXPECT_EQ("Changed", bodies[1]["propertyOp"].asString());
    EXPECT_FALSE(bodies[1]["active"].asBool());
    EXPECT_TRUE(bodies[1]["resync"].asBool());
    EXPECT_EQ("Deleted", bodies[2]["propertyOp"].asString());
    EXPECT_LT(bodies[0]["seq"].asUInt64(), bodies[1]["seq"].asUInt64());
}
