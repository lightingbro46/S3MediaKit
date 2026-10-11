#include <strings.h>
#include <time.h>
#include <json/json.h>
#include "Control/OnvifEventKafka.h"

using namespace std;
using namespace mediakit;

namespace managerkit {
namespace OnvifEventKafka {

static string trim(const string &s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return "";
    }
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

int normalizeActive(const string &value) {
    // ONVIF uses xs:boolean for IsMotion/IsTamper/IsInside/State; LogicalState of a digital input or
    // relay is "true"/"false" or "active"/"inactive"; some cameras send "on"/"off" or "closed"/"open"
    // for contacts.
    static const char *s_true[] = { "true", "1", "active", "on", "closed", "yes" };
    static const char *s_false[] = { "false", "0", "inactive", "off", "open", "opened", "no" };
    auto v = trim(value);
    for (auto t : s_true) {
        if (strcasecmp(v.data(), t) == 0) {
            return 1;
        }
    }
    for (auto f : s_false) {
        if (strcasecmp(v.data(), f) == 0) {
            return 0;
        }
    }
    return -1;
}

static string isoUtc(time_t t) {
    if (!t) {
        return "";
    }
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

static Json::Value itemsObject(const OnvifSimpleItems &items) {
    Json::Value ret(Json::objectValue);
    for (auto &item : items) {
        ret[item.first] = item.second;
    }
    return ret;
}

EventRecord toRecord(const OnvifEventMessage &event, const Context &ctx, bool resync) {
    const bool state = !event.stateItem.empty();
    Json::Value body(Json::objectValue);
    body["schemaVersion"] = kSchemaVersion;
    body["eventId"] = ctx.mediaServerId + ":" + ctx.bootId + ":" + to_string(ctx.seq);
    body["seq"] = Json::UInt64(ctx.seq);
    body["source"] = "onvif";
    body["mediaServerId"] = ctx.mediaServerId;
    body["projectId"] = ctx.projectId;
    body["deviceId"] = ctx.deviceId;
    body["eventType"] = event.eventType;
    body["kind"] = state ? "state" : "pulse";
    if (state) {
        int active = normalizeActive(event.stateValue());
        body["active"] = active < 0 ? Json::Value(Json::nullValue) : Json::Value(active == 1);
    }
    body["propertyOp"] = resync ? "Changed" : event.propertyOp;
    body["resync"] = resync;
    body["topic"] = event.topic;
    body["cameraTime"] = isoUtc(event.cameraUtc);
    body["serverTimeMs"] = Json::Int64(ctx.serverTimeMs);
    body["lagMs"] = Json::Int64(ctx.lagMs);
    body["sourceItems"] = itemsObject(event.source);
    body["data"] = itemsObject(event.data);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;

    EventRecord ret;
    ret.key = ctx.deviceId;
    ret.headers = { { "schema", "device-event" },
                    { "v", to_string(kSchemaVersion) },
                    { "source", "onvif" },
                    { "eventType", event.eventType },
                    { "mediaServerId", ctx.mediaServerId } };
    ret.value = Json::writeString(writer, body);
    return ret;
}

Decision decide(const OnvifEventMessage &event, const SentStates &sent) {
    if (event.propertyOp != "Initialized") {
        return Decision::Send;
    }
    auto it = sent.find(event.stateKey());
    if (it == sent.end() || it->second == event.stateValue()) {
        // Nothing sent for that state: the consumer's view is unknown, nothing to correct.
        return Decision::Drop;
    }
    return Decision::SendResync;
}

void remember(const OnvifEventMessage &event, SentStates &sent, size_t max_keys) {
    if (event.stateItem.empty() || event.propertyOp.empty()) {
        return;
    }
    auto key = event.stateKey();
    if (event.propertyOp == "Deleted") {
        sent.erase(key);
        return;
    }
    auto it = sent.find(key);
    if (it != sent.end()) {
        it->second = event.stateValue();
    } else if (!max_keys || sent.size() < max_keys) {
        sent.emplace(std::move(key), event.stateValue());
    }
}

} // namespace OnvifEventKafka
} // namespace managerkit
