#include <gtest/gtest.h>
#include <json/json.h>
#include "Control/OnvifEventKafka.h"
#include "Onvif/OnvifEventTopics.h"

using namespace managerkit;
using namespace mediakit;

namespace {

OnvifEventMessage makeEvent(const std::string &topic, const std::string &op, const std::string &value,
                            const std::string &source_token = "VSC_1") {
    OnvifEventMessage ev;
    ev.topic = "tns1:" + topic;
    auto rule = OnvifEventTopics::classify(ev.topic);
    if (rule) {
        ev.eventType = rule->eventType;
        ev.stateItem = rule->stateItem;
    }
    ev.propertyOp = op;
    ev.utcTime = "2026-10-11T03:12:05Z";
    ev.cameraUtc = parseXsDateTime(ev.utcTime);
    ev.source = { { "VideoSourceConfigurationToken", source_token } };
    if (!ev.stateItem.empty()) {
        ev.data = { { ev.stateItem, value } };
    }
    return ev;
}

OnvifEventKafka::Context makeContext() {
    OnvifEventKafka::Context ctx;
    ctx.mediaServerId = "ms-01";
    ctx.projectId = "p-123";
    ctx.deviceId = "cam-1";
    ctx.bootId = "5f1c2a7e";
    ctx.seq = 4666;
    ctx.serverTimeMs = 1791688325180;
    ctx.lagMs = 180;
    return ctx;
}

Json::Value parse(const std::string &text) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errs;
    EXPECT_TRUE(reader->parse(text.data(), text.data() + text.size(), &root, &errs)) << errs << " in " << text;
    return root;
}

std::string header(const EventRecord &rec, const std::string &name) {
    for (auto &h : rec.headers) {
        if (h.first == name) return h.second;
    }
    return "<none>";
}

} // namespace

TEST(OnvifEventKafkaTest, NormalizesStateValues) {
    for (auto v : { "true", "TRUE", " 1 ", "active", "On", "closed", "yes" }) {
        EXPECT_EQ(1, OnvifEventKafka::normalizeActive(v)) << v;
    }
    for (auto v : { "false", "0", "Inactive", "off", "open", "opened", "no" }) {
        EXPECT_EQ(0, OnvifEventKafka::normalizeActive(v)) << v;
    }
    for (auto v : { "", "2", "maybe", "truee" }) {
        EXPECT_EQ(-1, OnvifEventKafka::normalizeActive(v)) << v;
    }
}

TEST(OnvifEventKafkaTest, StateEventMessage) {
    auto ev = makeEvent("RuleEngine/CellMotionDetector/Motion", "Changed", "true");
    ev.source.emplace_back("Rule", "MyMotionDetectorRule");
    auto rec = OnvifEventKafka::toRecord(ev, makeContext(), false);

    EXPECT_EQ("cam-1", rec.key);
    EXPECT_EQ("device-event", header(rec, "schema"));
    EXPECT_EQ("1", header(rec, "v"));
    EXPECT_EQ("onvif", header(rec, "source"));
    EXPECT_EQ("MOTION", header(rec, "eventType"));
    EXPECT_EQ("ms-01", header(rec, "mediaServerId"));
    EXPECT_EQ("<none>", header(rec, "replayed"));

    auto body = parse(rec.value);
    EXPECT_EQ(1, body["schemaVersion"].asInt());
    EXPECT_EQ("ms-01:5f1c2a7e:4666", body["eventId"].asString());
    EXPECT_EQ(4666u, body["seq"].asUInt64());
    EXPECT_EQ("onvif", body["source"].asString());
    EXPECT_EQ("p-123", body["projectId"].asString());
    EXPECT_EQ("cam-1", body["deviceId"].asString());
    EXPECT_EQ("MOTION", body["eventType"].asString());
    EXPECT_EQ("state", body["kind"].asString());
    EXPECT_TRUE(body["active"].asBool());
    EXPECT_EQ("Changed", body["propertyOp"].asString());
    EXPECT_FALSE(body["resync"].asBool());
    EXPECT_EQ("tns1:RuleEngine/CellMotionDetector/Motion", body["topic"].asString());
    EXPECT_EQ("2026-10-11T03:12:05Z", body["cameraTime"].asString());
    EXPECT_EQ(1791688325180, body["serverTimeMs"].asInt64());
    EXPECT_EQ(180, body["lagMs"].asInt64());
    EXPECT_EQ("VSC_1", body["sourceItems"]["VideoSourceConfigurationToken"].asString());
    EXPECT_EQ("MyMotionDetectorRule", body["sourceItems"]["Rule"].asString());
    EXPECT_EQ("true", body["data"]["IsMotion"].asString());
}

TEST(OnvifEventKafkaTest, EveryEventTypeHasKindAndActive) {
    struct Case {
        const char *topic;
        const char *type;
        bool state;
    } cases[] = {
        { "RuleEngine/CellMotionDetector/Motion", "MOTION", true },
        { "VideoSource/MotionAlarm", "MOTION", true },
        { "RuleEngine/TamperDetector/Tamper", "TAMPER", true },
        { "VideoSource/GlobalSceneChange/ImagingService", "TAMPER", true },
        { "VideoSource/SignalLoss", "VIDEO_LOSS", true },
        { "Device/Trigger/DigitalInput", "DIGITAL_INPUT", true },
        { "Device/Trigger/Relay", "RELAY", true },
        { "RuleEngine/FieldDetector/ObjectsInside", "INTRUSION", true },
        { "RuleEngine/LineDetector/Crossed", "LINE_CROSSING", false },
    };
    for (auto &c : cases) {
        SCOPED_TRACE(c.topic);
        auto ev = makeEvent(c.topic, c.state ? "Changed" : "", "false");
        auto body = parse(OnvifEventKafka::toRecord(ev, makeContext(), false).value);
        EXPECT_EQ(c.type, body["eventType"].asString());
        EXPECT_EQ(c.state ? "state" : "pulse", body["kind"].asString());
        if (c.state) {
            ASSERT_TRUE(body["active"].isBool());
            EXPECT_FALSE(body["active"].asBool());
        } else {
            EXPECT_FALSE(body.isMember("active"));
            EXPECT_EQ("", body["propertyOp"].asString());
        }
    }
}

TEST(OnvifEventKafkaTest, DeletedUnknownValueAndNoCameraTime) {
    auto ev = makeEvent("Device/Trigger/Relay", "Deleted", "");
    ev.data.clear();
    ev.utcTime.clear();
    ev.cameraUtc = 0;
    auto body = parse(OnvifEventKafka::toRecord(ev, makeContext(), false).value);
    EXPECT_EQ("Deleted", body["propertyOp"].asString());
    EXPECT_TRUE(body["active"].isNull());
    EXPECT_EQ("", body["cameraTime"].asString());
    EXPECT_TRUE(body["data"].isObject());
    EXPECT_EQ(0u, body["data"].size());

    auto odd = makeEvent("Device/Trigger/DigitalInput", "Changed", "half-open");
    EXPECT_TRUE(parse(OnvifEventKafka::toRecord(odd, makeContext(), false).value)["active"].isNull());
}

TEST(OnvifEventKafkaTest, SpecialCharactersStayValidJson) {
    auto ev = makeEvent("RuleEngine/CellMotionDetector/Motion", "Changed", "true",
                        "a\"b\\c\n\x01 Cổng chính");
    ev.data.emplace_back("Note", "<x>&'\"");
    auto rec = OnvifEventKafka::toRecord(ev, makeContext(), false);
    auto body = parse(rec.value);
    EXPECT_EQ("a\"b\\c\n\x01 Cổng chính", body["sourceItems"]["VideoSourceConfigurationToken"].asString());
    EXPECT_EQ("<x>&'\"", body["data"]["Note"].asString());
    EXPECT_EQ(std::string::npos, rec.value.find('\n')); // one line
}

TEST(OnvifEventKafkaTest, ResyncIsSentAsChanged) {
    auto ev = makeEvent("RuleEngine/CellMotionDetector/Motion", "Initialized", "false");
    auto body = parse(OnvifEventKafka::toRecord(ev, makeContext(), true).value);
    EXPECT_EQ("Changed", body["propertyOp"].asString());
    EXPECT_TRUE(body["resync"].asBool());
    EXPECT_FALSE(body["active"].asBool());
}

TEST(OnvifEventKafkaTest, InitializedOnlySentWhenItDiffersFromTheValueSent) {
    using OnvifEventKafka::Decision;
    OnvifEventKafka::SentStates sent;
    auto init_true = makeEvent("RuleEngine/CellMotionDetector/Motion", "Initialized", "true");

    // Nothing sent yet: dropped and not remembered.
    EXPECT_EQ(Decision::Drop, OnvifEventKafka::decide(init_true, sent));
    EXPECT_TRUE(sent.empty());

    auto changed_true = makeEvent("RuleEngine/CellMotionDetector/Motion", "Changed", "true");
    EXPECT_EQ(Decision::Send, OnvifEventKafka::decide(changed_true, sent));
    OnvifEventKafka::remember(changed_true, sent, 0);
    ASSERT_EQ(1u, sent.size());

    EXPECT_EQ(Decision::Drop, OnvifEventKafka::decide(init_true, sent));
    auto init_false = makeEvent("RuleEngine/CellMotionDetector/Motion", "Initialized", "false");
    EXPECT_EQ(Decision::SendResync, OnvifEventKafka::decide(init_false, sent));
    OnvifEventKafka::remember(init_false, sent, 0);
    EXPECT_EQ("false", sent.begin()->second);
    EXPECT_EQ(Decision::Drop, OnvifEventKafka::decide(init_false, sent));

    // Another video source is another state.
    auto other = makeEvent("RuleEngine/CellMotionDetector/Motion", "Initialized", "true", "VSC_2");
    EXPECT_EQ(Decision::Drop, OnvifEventKafka::decide(other, sent));

    // Deleted forgets the state.
    auto deleted = makeEvent("RuleEngine/CellMotionDetector/Motion", "Deleted", "");
    EXPECT_EQ(Decision::Send, OnvifEventKafka::decide(deleted, sent));
    OnvifEventKafka::remember(deleted, sent, 0);
    EXPECT_TRUE(sent.empty());
    EXPECT_EQ(Decision::Drop, OnvifEventKafka::decide(init_false, sent));
}

TEST(OnvifEventKafkaTest, PulseEventsAreNotRemembered) {
    OnvifEventKafka::SentStates sent;
    auto crossed = makeEvent("RuleEngine/LineDetector/Crossed", "", "");
    EXPECT_EQ(OnvifEventKafka::Decision::Send, OnvifEventKafka::decide(crossed, sent));
    OnvifEventKafka::remember(crossed, sent, 0);
    EXPECT_TRUE(sent.empty());
}

TEST(OnvifEventKafkaTest, KeyLimitPerCamera) {
    OnvifEventKafka::SentStates sent;
    for (int i = 0; i < 5; ++i) {
        OnvifEventKafka::remember(
            makeEvent("RuleEngine/CellMotionDetector/Motion", "Changed", "true", "VSC_" + std::to_string(i)), sent, 3);
    }
    EXPECT_EQ(3u, sent.size());
    // Known keys still update when full.
    auto update = makeEvent("RuleEngine/CellMotionDetector/Motion", "Changed", "false", "VSC_0");
    OnvifEventKafka::remember(update, sent, 3);
    EXPECT_EQ("false", sent[update.stateKey()]);
}
