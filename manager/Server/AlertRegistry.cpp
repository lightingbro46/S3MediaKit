#include "AlertRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Common/config.h"
#include "Util/NoticeCenter.h"
#include "Util/util.h"
#include "ResourceMonitor.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

static Json::Value jsonNumber(double v) {
    if (std::isnan(v) || std::isinf(v)) {
        return Json::Value(Json::nullValue);
    }
    return Json::Value(std::round(v * 100.0) / 100.0);
}

static const char *levelString(AlertRegistry::Level level) {
    switch (level) {
        case AlertRegistry::CRITICAL: return "critical";
        case AlertRegistry::WARNING: return "warning";
        default: return "recovered";
    }
}

Json::Value AlertRegistry::Alert::toJson() const {
    Json::Value v = extra.isObject() ? extra : Json::Value(Json::objectValue);
    v["seq"] = (Json::UInt64)seq;
    v["eventCode"] = event_code;
    v["family"] = family;
    v["entity"] = entity;
    v["level"] = levelString(level);
    v["since"] = (Json::Int64)since;
    v["at"] = (Json::Int64)at;
    v["currentValue"] = jsonNumber(value);
    v["threshold"] = jsonNumber(threshold);
    return v;
}

const size_t AlertRegistry::kRecentCapacity;

AlertRegistry &AlertRegistry::Instance() {
    // Never destroyed: broadcasts may still arrive during exit.
    static AlertRegistry *s_instance = new AlertRegistry();
    return *s_instance;
}

AlertRegistry::AlertRegistry()
    : _seq(getCurrentMillisecond(true)) {}

void AlertRegistry::onEvent(const string &family, const string &entity, const string &event_code, Level level,
                            double value, double threshold, const Json::Value &extra, int64_t now) {
    if (now <= 0) {
        now = (int64_t)time(nullptr);
    }
    lock_guard<mutex> lck(_mtx);
    Alert a;
    a.seq = ++_seq;
    a.event_code = event_code;
    a.family = family;
    a.entity = entity;
    a.level = level;
    a.since = now;
    a.at = now;
    a.value = value;
    a.threshold = threshold;
    a.extra = extra;

    const string key = family + "|" + entity;
    auto it = _active.find(key);
    if (level == RECOVERED) {
        if (it != _active.end()) {
            a.since = it->second.since;
            _active.erase(it);
        }
    } else {
        if (it != _active.end()) {
            // Repeats and level changes keep the time the problem started.
            a.since = it->second.since;
        }
        _active[key] = a;
    }
    _recent.push_back(std::move(a));
    while (_recent.size() > kRecentCapacity) {
        _recent.pop_front();
    }
}

void AlertRegistry::addEvent(const string &family, const string &entity, const string &event_code, Level level,
                             double value, double threshold, const Json::Value &extra, int64_t now) {
    if (now <= 0) {
        now = (int64_t)time(nullptr);
    }
    lock_guard<mutex> lck(_mtx);
    Alert a;
    a.seq = ++_seq;
    a.event_code = event_code;
    a.family = family;
    a.entity = entity;
    a.level = level;
    a.since = now;
    a.at = now;
    a.value = value;
    a.threshold = threshold;
    a.extra = extra;
    _recent.push_back(std::move(a));
    while (_recent.size() > kRecentCapacity) {
        _recent.pop_front();
    }
}

bool AlertRegistry::isActive(const string &family, const string &entity) const {
    lock_guard<mutex> lck(_mtx);
    return _active.count(family + "|" + entity) > 0;
}

Json::Value AlertRegistry::toJson(uint64_t since_seq, size_t max_recent) const {
    lock_guard<mutex> lck(_mtx);
    Json::Value out;
    out["seq"] = (Json::UInt64)_seq;
    out["active"] = Json::arrayValue;
    vector<const Alert *> active;
    for (const auto &kv : _active) {
        active.push_back(&kv.second);
    }
    // Most severe first, then oldest.
    std::sort(active.begin(), active.end(), [](const Alert *a, const Alert *b) {
        if (a->level != b->level) {
            return a->level > b->level;
        }
        return a->since < b->since;
    });
    for (auto a : active) {
        out["active"].append(a->toJson());
    }
    out["recent"] = Json::arrayValue;
    size_t start = 0;
    size_t matching = 0;
    for (const auto &a : _recent) {
        if (a.seq > since_seq) {
            ++matching;
        }
    }
    // Keep the newest `max_recent` of the matching ones.
    size_t skip = matching > max_recent ? matching - max_recent : 0;
    for (const auto &a : _recent) {
        if (a.seq <= since_seq) {
            continue;
        }
        if (start++ < skip) {
            continue;
        }
        out["recent"].append(a.toJson());
    }
    return out;
}

size_t AlertRegistry::activeCount() const {
    lock_guard<mutex> lck(_mtx);
    return _active.size();
}

size_t AlertRegistry::activeCount(Level min_level) const {
    lock_guard<mutex> lck(_mtx);
    size_t n = 0;
    for (const auto &kv : _active) {
        n += kv.second.level >= min_level ? 1 : 0;
    }
    return n;
}

void AlertRegistry::setManagedDiskThresholds(const map<string, pair<double, double> > &thresholds) {
    lock_guard<mutex> lck(_mtx);
    _managed_disk = thresholds;
}

bool AlertRegistry::filterDiskUsage(const string &mount_point, double usage, Level &level, double &threshold) const {
    lock_guard<mutex> lck(_mtx);
    auto it = _managed_disk.find(mount_point);
    if (it == _managed_disk.end()) {
        return true;
    }
    if (usage < it->second.first) {
        return false;
    }
    if (level == CRITICAL && usage < it->second.second) {
        level = WARNING;
    }
    threshold = level == CRITICAL ? it->second.second : it->second.first;
    return true;
}

void AlertRegistry::clear() {
    lock_guard<mutex> lck(_mtx);
    _active.clear();
    _recent.clear();
    _managed_disk.clear();
    _seq = 0;
}

// ── Broadcast listeners ──────────────────────────────────────────
// eventCode values are the ones of the system-event webhook (server/WebHook.cpp,
// docs/api-system-alert-events.md); keep both in sync.

static const char *systemFamily(uint8_t type) {
    switch (static_cast<ResourceType>(type)) {
        case ResourceType::CPU: return "Cpu";
        case ResourceType::MEMORY: return "Ram";
        case ResourceType::HDD: return "Disk";
        case ResourceType::NETWORK: return "NetWork";
        case ResourceType::READER: return "LiveStreamLimit";
        default: return "Unknown";
    }
}

static int s_tag;

void AlertRegistry::start() {
    {
        lock_guard<mutex> lck(_mtx);
        if (_started) {
            return;
        }
        _started = true;
    }
    auto &nc = NoticeCenter::Instance();
    // Listeners only lock a mutex: they never throw or block the webhook listeners next to them.
    nc.addListener(&s_tag, Broadcast::kBroadcastSystemAlert, [](BroadcastSystemAlertArgs) {
        string family = systemFamily(type);
        string code = static_cast<ResourceType>(type) == ResourceType::READER
                          ? string("LiveStreamLimitExceeded")
                          : family + (is_critical ? "Critical" : "Warning");
        AlertRegistry::Instance().onEvent(family, "", code, is_critical ? CRITICAL : WARNING, usage, threshold);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastSystemAlertRecovered, [](BroadcastSystemAlertRecoveredArgs) {
        string family = systemFamily(type);
        string code = static_cast<ResourceType>(type) == ResourceType::READER ? string("LiveStreamLimitRecovered")
                                                                                : family + "Recovered";
        AlertRegistry::Instance().onEvent(family, "", code, RECOVERED, usage, threshold);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastDiskUsageAlert, [](BroadcastDiskUsageAlertArgs) {
        Json::Value extra;
        extra["mountPoint"] = mount_point;
        extra["device"] = device;
        // No recovery broadcast exists for a mount: the health monitor closes it (DiskRecovered)
        // once the mount is back under the warning threshold.
        Level level = is_critical ? CRITICAL : WARNING;
        double th = threshold;
        if (!AlertRegistry::Instance().filterDiskUsage(mount_point, usage, level, th)) {
            return;
        }
        AlertRegistry::Instance().onEvent("DiskUsage", mount_point, level == CRITICAL ? "DiskCritical" : "DiskWarning",
                                          level, usage, th, extra);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastDiskIoAlert, [](BroadcastDiskIoAlertArgs) {
        Json::Value extra;
        extra["mountPoint"] = mount_point;
        extra["device"] = device;
        extra["metric"] = metric;
        extra["isMainStorage"] = is_main_storage;
        string code = level == 0 ? "DiskIoRecovered" : (level >= 2 ? "DiskIoCritical" : "DiskIoWarning");
        AlertRegistry::Instance().onEvent("DiskIo", mount_point, code,
                                          level == 0 ? RECOVERED : (level >= 2 ? CRITICAL : WARNING), value, threshold, extra);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastDiskWriteAlert, [](BroadcastDiskWriteAlertArgs) {
        Json::Value extra;
        extra["path"] = path;
        extra["mountPoint"] = mount_point;
        extra["device"] = device;
        extra["isMainStorage"] = is_main_storage;
        extra["error"] = error ? strerror(error) : "";
        string code = state == 0 ? "DiskWritable" : (state == 1 ? "DiskReadOnly" : "DiskWriteFailed");
        AlertRegistry::Instance().onEvent("DiskWrite", path, code, state == 0 ? RECOVERED : CRITICAL, state, 0, extra);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastPoolThroughputAlert, [](BroadcastPoolThroughputAlertArgs) {
        Json::Value extra;
        extra["poolId"] = pool_id;
        extra["poolName"] = pool_name;
        extra["tier"] = tier;
        extra["demandMbps"] = jsonNumber(demand_mbps);
        extra["backlogBytes"] = (Json::Int64)backlog_bytes;
        extra["backlogEtaSec"] = (Json::Int64)eta_sec;
        AlertRegistry::Instance().onEvent("PoolThroughput", pool_id, is_low ? "PoolThroughputLow" : "PoolThroughputRecovered",
                                          is_low ? WARNING : RECOVERED, capacity_mbps, required_mbps, extra);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastRetentionForecastAlert, [](BroadcastRetentionForecastAlertArgs) {
        Json::Value extra;
        extra["scope"] = scope;
        extra["poolId"] = pool_id;
        extra["poolName"] = pool_name;
        extra["path"] = path;
        extra["mountPoint"] = mount_point;
        extra["ingestMbps"] = jsonNumber(ingest_mbps);
        AlertRegistry::Instance().onEvent("RetentionCapacity", scope + "|" + pool_id + "|" + path,
                                          level > 0 ? "RetentionCapacityLow" : "RetentionCapacityRecovered",
                                          level > 1 ? CRITICAL : (level > 0 ? WARNING : RECOVERED),
                                          retention_days, required_days, extra);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastEventBusAlert, [](BroadcastEventBusAlertArgs) {
        Json::Value extra;
        extra["reason"] = reason;
        extra["pendingEvents"] = (Json::Int64)pending_events;
        extra["spoolBytes"] = (Json::Int64)spool_bytes;
        extra["droppedEvents"] = (Json::Int64)dropped_events;
        Level level = event_code == "EventBusRecovered" ? RECOVERED
                    : (event_code == "EventBusSpoolFull" ? CRITICAL : WARNING);
        AlertRegistry::Instance().onEvent("EventBus", "", event_code, level, (double)current_value, (double)threshold, extra);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastStreamReaderAlert, [](BroadcastStreamReaderAlertArgs) {
        Json::Value extra;
        extra["cameraId"] = camera_id;
        AlertRegistry::Instance().onEvent("CameraStreamLimit", camera_id, "CameraStreamLimitExceeded",
                                          is_critical ? CRITICAL : WARNING, usage, threshold, extra);
    });
    nc.addListener(&s_tag, Broadcast::kBroadcastStreamReaderAlertRecovered, [](BroadcastStreamReaderAlertRecoveredArgs) {
        Json::Value extra;
        extra["cameraId"] = camera_id;
        AlertRegistry::Instance().onEvent("CameraStreamLimit", camera_id, "CameraStreamLimitRecovered", RECOVERED,
                                          usage, threshold, extra);
    });
}

} // namespace managerkit
