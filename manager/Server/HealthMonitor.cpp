#include "HealthMonitor.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include "Common/config.h"
#include "Codec/TranscodeStats.h"
#include "Camera/GenericRtspCameraImp.h"
#include "Common/DeviceSource.h"
#include "Util/NoticeCenter.h"
#ifdef ENABLE_MP4
#include "Record/MP4Recorder.h"
#endif
#include "AlertRegistry.h"
#include "Thread/WorkThreadPool.h"
#include "Util/onceToken.h"
#include "GlobalMonitor.h"
#include "HealthProbe.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

// ──────────────────────────────────────────────────────────────────
// Config
// ──────────────────────────────────────────────────────────────────
namespace HealthMonitorConfig {

#define HM_FIELD "monitor."
const string kEnable = HM_FIELD "enable";
const string kIntervalSec = HM_FIELD "intervalSec";
const string kSustainSamples = HM_FIELD "sustainSamples";
const string kGpuFallbackWindowSec = HM_FIELD "gpuFallbackWindowSec";
const string kRecordStaleSec = HM_FIELD "recordStaleSec";
const string kPlayWindowSec = HM_FIELD "playWindowSec";
const string kPlayMinRequests = HM_FIELD "playMinRequests";

const vector<ThresholdDef> &thresholdDefs() {
    static const vector<ThresholdDef> defs = {
        // host (4.1)
        { "load1PerCore", "cpu.load1_per_core", 1.5, 3, true },
        { "swapUsedPct", "swap.used_pct", 20, 50, true },
        { "netUtilPct", "net.util_pct", 70, 90, true },
        { "netErrorsPerSec", "net.errors_per_sec", 0.01, 100, true },
        { "netDropsPerSec", "net.drops_per_sec", 10, 100, true },
        { "fdUsedPct", "proc.fd_used_pct", 70, 90, true },
        // process (4.2)
        { "pollerDelayMs", "pollers.delay_max_ms", 50, 200, true },
        { "pollerLoadPct", "pollers.load_max", 80, 95, true },
        { "workPollerDelayMs", "work_pollers.delay_max_ms", 100, 500, true },
        { "blockingWaitMs", "blocking.wait_max_ms", 1000, 5000, true },
        { "blockingBusyPct", "blocking.busy_pct", 80, 95, true },
        // recording (4.4): any stalled camera is a warning, 5 % of the expected ones critical
        { "recordStalledPct", "recording.stalled_pct", 0.01, 5, true },
        // delivery (4.5)
        { "viewerLimitPct", "viewers.limit_used_pct", 80, 95, true },
        { "playFailurePct", "play.failure_pct", 5, 20, true },
    };
    return defs;
}

string warnKey(const ThresholdDef &def) {
    return string(HM_FIELD) + def.name + "Warn";
}

string critKey(const ThresholdDef &def) {
    return string(HM_FIELD) + def.name + "Crit";
}

static onceToken token([]() {
    mINI::Instance()[kEnable] = 1;
    mINI::Instance()[kIntervalSec] = 10;
    mINI::Instance()[kSustainSamples] = 3;
    mINI::Instance()[kGpuFallbackWindowSec] = 300;
    mINI::Instance()[kRecordStaleSec] = 0;
    mINI::Instance()[kPlayWindowSec] = 300;
    mINI::Instance()[kPlayMinRequests] = 20;
    for (const auto &def : thresholdDefs()) {
        mINI::Instance()[warnKey(def)] = def.warn;
        mINI::Instance()[critKey(def)] = def.crit;
    }
});

HealthThreshold threshold(const string &name) {
    HealthThreshold th;
    for (const auto &def : thresholdDefs()) {
        if (name != def.name) {
            continue;
        }
        th.warn = def.warn;
        th.crit = def.crit;
        th.higher_is_worse = def.higher_is_worse;
        // Read on every cycle (a handful of keys every 10 s), so config/set applies at the next one.
        auto &ini = mINI::Instance();
        auto it = ini.find(warnKey(def));
        if (it != ini.end() && !it->second.empty()) {
            th.warn = it->second.as<double>();
        }
        it = ini.find(critKey(def));
        if (it != ini.end() && !it->second.empty()) {
            th.crit = it->second.as<double>();
        }
        break;
    }
    return th;
}

// GlobalMonitor thresholds editable from the monitor config API, as warn/crit pairs.
static const vector<pair<string, string> > &globalPairs() {
    using namespace GlobalMonitorConfig;
    static const vector<pair<string, string> > pairs = {
        { kCpuWarningThreshold, kCpuCriticalThreshold },
        { kMemoryWarningThreshold, kMemoryCriticalThreshold },
        { kHddWarningThreshold, kHddCriticalThreshold },
        { kDiskIoUtilWarningThreshold, kDiskIoUtilCriticalThreshold },
        { kDiskIoAwaitWarningMs, kDiskIoAwaitCriticalMs },
        { kIowaitWarningThreshold, kIowaitCriticalThreshold },
        { kDiskIoPsiFullWarningThreshold, kDiskIoPsiFullCriticalThreshold },
    };
    return pairs;
}

const vector<pair<string, string> > &thresholdPairs() {
    static const vector<pair<string, string> > pairs = []() {
        vector<pair<string, string> > out;
        for (const auto &def : thresholdDefs()) {
            out.emplace_back(warnKey(def), critKey(def));
        }
        for (const auto &pr : globalPairs()) {
            out.push_back(pr);
        }
        return out;
    }();
    return pairs;
}

// Integer keys and their minimum.
static const map<string, int> &countKeys() {
    static const map<string, int> keys = {
        { kEnable, 0 },
        { kIntervalSec, 1 },
        { kSustainSamples, 1 },
        { kGpuFallbackWindowSec, 0 },
        { kRecordStaleSec, 0 },
        { kPlayWindowSec, 10 },
        { kPlayMinRequests, 1 },
        { GlobalMonitorConfig::kDiskIoSustainSamples, 1 },
    };
    return keys;
}

const vector<string> &editableKeys() {
    static const vector<string> keys = []() {
        vector<string> out;
        for (const auto &kv : countKeys()) {
            out.push_back(kv.first);
        }
        for (const auto &pr : thresholdPairs()) {
            out.push_back(pr.first);
            out.push_back(pr.second);
        }
        return out;
    }();
    return keys;
}

static bool parseNumber(const string &text, double &out) {
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    out = strtod(text.c_str(), &end);
    while (end && *end == ' ') {
        ++end;
    }
    return end && *end == '\0' && !std::isnan(out) && !std::isinf(out);
}

string validateChanges(const map<string, string> &changes, const function<string(const string &)> &current) {
    const auto &keys = editableKeys();
    for (const auto &kv : changes) {
        if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) {
            return "key not editable: " + kv.first;
        }
        double v = 0;
        if (!parseNumber(kv.second, v)) {
            return "not a number: " + kv.first;
        }
        auto it = countKeys().find(kv.first);
        if (it != countKeys().end()) {
            if (v != std::floor(v) || v < it->second) {
                return "must be an integer >= " + to_string(it->second) + ": " + kv.first;
            }
            if (kv.first == kEnable && v > 1) {
                return "must be 0 or 1: " + kv.first;
            }
        } else if (v < 0) {
            return "must be >= 0: " + kv.first;
        }
    }
    auto value = [&](const string &key) {
        auto it = changes.find(key);
        return it != changes.end() ? it->second : current(key);
    };
    for (const auto &pr : thresholdPairs()) {
        if (!changes.count(pr.first) && !changes.count(pr.second)) {
            continue;
        }
        double warn = 0, crit = 0;
        if (!parseNumber(value(pr.first), warn) || !parseNumber(value(pr.second), crit)) {
            return "not a number: " + pr.first + "/" + pr.second;
        }
        if (warn >= crit) {
            return "warning must be lower than critical: " + pr.first + " < " + pr.second;
        }
    }
    return string();
}

} // namespace HealthMonitorConfig

// ──────────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────────

static Json::Value num(double v) {
    if (std::isnan(v) || std::isinf(v)) {
        return Json::Value(Json::nullValue);
    }
    return Json::Value(std::round(v * 100.0) / 100.0);
}

static HealthLevel fromDiskIoLevel(DiskIoLevel level) {
    switch (level) {
        case DiskIoLevel::CRITICAL: return HealthLevel::CRITICAL;
        case DiskIoLevel::WARNING: return HealthLevel::WARNING;
        default: return HealthLevel::OK;
    }
}

// GlobalMonitor threshold, or its config default when the monitor has none (-1).
static HealthThreshold globalThreshold(ResourceType type, const string &warn_key, const string &crit_key) {
    auto pr = GlobalMonitor::Instance().getThreshold(type);
    HealthThreshold th;
    th.warn = pr.first >= 0 ? pr.first : mINI::Instance()[warn_key].as<double>();
    th.crit = pr.second >= 0 ? pr.second : mINI::Instance()[crit_key].as<double>();
    return th;
}

const char *recordingStateString(RecordingState state) {
    switch (state) {
        case RecordingState::STARTING: return "starting";
        case RecordingState::ACTIVE: return "active";
        case RecordingState::STALLED: return "stalled";
        default: return "not_expected";
    }
}

RecordingState evaluateRecording(bool expected, int64_t expected_since, int64_t last_write, int64_t now, int stale_sec) {
    bool written = last_write > 0 && now - last_write <= stale_sec;
    if (!expected) {
        return written ? RecordingState::ACTIVE : RecordingState::NOT_EXPECTED;
    }
    if (written) {
        return RecordingState::ACTIVE;
    }
    // Grace after recording became expected (schedule hour starts, motion begins, camera enabled):
    // the first segment closes only after mp4_max_second.
    if (now - expected_since < stale_sec) {
        return RecordingState::STARTING;
    }
    return RecordingState::STALLED;
}

int defaultRecordStaleSec(int mp4_max_second) {
    return (int)(std::max(1, mp4_max_second) * 1.5) + 60;
}

static int recordStaleSec() {
    GET_CONFIG(int, stale_sec, HealthMonitorConfig::kRecordStaleSec);
    if (stale_sec > 0) {
        return stale_sec;
    }
    GET_CONFIG(int, max_second, Protocol::kMP4MaxSecond);
    return defaultRecordStaleSec(max_second);
}

// ──────────────────────────────────────────────────────────────────
// HealthMonitor
// ──────────────────────────────────────────────────────────────────

HealthMonitor &HealthMonitor::Instance() {
    // Never destroyed: timers and executor callbacks may still call Instance() during exit.
    static HealthMonitor *s_instance = new HealthMonitor();
    return *s_instance;
}

HealthMonitor::HealthMonitor() {
    _started_at = (int64_t)time(nullptr);
}

void HealthMonitor::start() {
    GET_CONFIG(bool, enable, HealthMonitorConfig::kEnable);
    if (!enable) {
        InfoL << "Health monitor disabled (" << HealthMonitorConfig::kEnable << "=0)";
        return;
    }
    if (_running.exchange(true)) {
        return;
    }
    GET_CONFIG(float, interval_sec, HealthMonitorConfig::kIntervalSec);
    if (interval_sec < 1) {
        interval_sec = 10;
    }
    AlertRegistry::Instance().start();
#ifdef ENABLE_MP4
    // Only locks a mutex: it must not throw or block the listeners after it (tier, encryption).
    static int s_tag;
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        HealthMonitor::Instance().onRecordWritten(info.app, info.file_size);
    });
#endif
    _poller = EventPollerPool::Instance().getPoller();
    _poller->async([]() { HealthMonitor::Instance().collectNow(); });
    _timer = std::make_shared<Timer>(interval_sec, []() {
        auto &self = HealthMonitor::Instance();
        if (!self._running.load()) {
            return false;
        }
        self.collectNow();
        return true;
    }, _poller);
    InfoL << "Health monitor started, every " << interval_sec << " s";
}

void HealthMonitor::stop() {
    _running = false;
    _timer.reset();
}

HealthSnapshot::Ptr HealthMonitor::snapshot() const {
    lock_guard<mutex> lck(_snap_mtx);
    return _snapshot;
}

bool HealthMonitor::blockingStats(vector<BlockingExecutors::PoolStats> &out) const {
    if (!_running.load()) {
        return false;
    }
    lock_guard<mutex> lck(_snap_mtx);
    out = _blocking_stats;
    return true;
}

void HealthMonitor::onRecordWritten(const string &camera_id, uint64_t bytes) {
    lock_guard<mutex> lck(_record_mtx);
    auto &w = _record_writes[camera_id];
    w.last_write = (int64_t)time(nullptr);
    w.bytes += bytes;
}

void HealthMonitor::onPlayResult(const string &err) {
    lock_guard<mutex> lck(_play_mtx);
    ++_play_current.requests;
    if (!err.empty()) {
        ++_play_current.failures;
        ++_play_failure_reasons[err];
    }
}

void HealthMonitor::requestPollerDelays() {
    int64_t now_ms = steadyNowMs();
    bool ask_pollers = false, ask_work = false;
    {
        lock_guard<mutex> lck(_delay_mtx);
        // One request in flight at a time: a stuck poller must not make requests pile up.
        if (_pollers.requested_ms <= _pollers.sampled_ms) {
            _pollers.requested_ms = now_ms;
            ask_pollers = true;
        }
        if (_work_pollers.requested_ms <= _work_pollers.sampled_ms) {
            _work_pollers.requested_ms = now_ms;
            ask_work = true;
        }
    }
    if (ask_pollers) {
        EventPollerPool::Instance().getExecutorDelay([](const vector<int> &delays) {
            HealthMonitor::Instance().onPollerDelay(false, delays, EventPollerPool::Instance().getExecutorLoad());
        });
    }
    if (ask_work) {
        WorkThreadPool::Instance().getExecutorDelay([](const vector<int> &delays) {
            HealthMonitor::Instance().onPollerDelay(true, delays, WorkThreadPool::Instance().getExecutorLoad());
        });
    }
}

void HealthMonitor::onPollerDelay(bool work_pool, const vector<int> &delays, const vector<int> &loads) {
    PollerDelay d;
    d.valid = !delays.empty();
    d.count = delays.size();
    long sum = 0;
    for (auto v : delays) {
        d.delay_max_ms = std::max(d.delay_max_ms, v);
        sum += v;
    }
    d.delay_avg_ms = delays.empty() ? 0 : (double)sum / delays.size();
    for (auto v : loads) {
        d.load_max = std::max(d.load_max, v);
    }
    lock_guard<mutex> lck(_delay_mtx);
    auto &slot = work_pool ? _work_pollers : _pollers;
    d.requested_ms = slot.requested_ms;
    d.sampled_ms = std::max(steadyNowMs(), slot.requested_ms);
    slot = d;
}

double HealthMonitor::effectiveDelayMs(const PollerDelay &d, int64_t now_ms) {
    double v = d.valid ? (double)d.delay_max_ms : NAN;
    if (d.requested_ms > d.sampled_ms) {
        double waiting = (double)(now_ms - d.requested_ms);
        // An answer normally comes back within milliseconds; one still missing after a second
        // means some poller is stuck, and the waiting time is a lower bound of its delay.
        if (waiting >= 1000) {
            v = std::isnan(v) ? waiting : std::max(v, waiting);
        }
    }
    return v;
}

void HealthMonitor::collectNow() {
    int64_t now = (int64_t)time(nullptr);
    ++_cycle;
    _hysteresis.setCycle(_cycle);
    GET_CONFIG(int, sustain, HealthMonitorConfig::kSustainSamples);
    HealthEvaluator ev(_hysteresis, sustain, now);
    Json::Value summary(Json::objectValue);

    try {
        collectHost(ev, summary, now);
        collectProcess(ev, summary, now);
        collectStorage(ev, summary, now);
        collectRecording(ev, summary, now);
        collectDelivery(ev, summary, now);
    } catch (const std::exception &ex) {
        WarnL << "Health monitor collect failed: " << ex.what();
    }
    // The answer is evaluated in the next cycle.
    requestPollerDelays();

    auto eval = ev.finish();
    _hysteresis.sweep(_cycle);
    auto sig = eval.signature();
    if (sig != _last_signature) {
        ++_rev;
        _last_signature = sig;
    }

    GET_CONFIG(string, media_server_id, General::kMediaServerId);
    Json::Value components(Json::objectValue);
    for (const auto &kv : eval.components) {
        components[kv.first] = kv.second.enabled ? healthLevelString(kv.second.level) : "disabled";
    }
    Json::Value reasons(Json::arrayValue);
    for (const auto &r : eval.reasons) {
        reasons.append(r.toJson());
    }

    auto snap = std::make_shared<HealthSnapshot>();
    snap->generated_at = now;
    snap->rev = _rev;
    Json::Value &h = snap->health;
    h["schemaVersion"] = 1;
    h["mediaServerId"] = media_server_id;
    h["generatedAt"] = (Json::Int64)now;
    h["rev"] = (Json::UInt64)_rev;
    h["overall"] = healthLevelString(eval.overall);
    h["score"] = eval.score;
    h["uptimeSec"] = (Json::Int64)(now - _started_at);
    h["components"] = components;
    h["reasons"] = reasons;
    h["alerts"]["active"] = (Json::UInt64)AlertRegistry::Instance().activeCount();
    h["alerts"]["critical"] = (Json::UInt64)AlertRegistry::Instance().activeCount(AlertRegistry::CRITICAL);

    summary["schemaVersion"] = 1;
    summary["mediaServerId"] = media_server_id;
    summary["generatedAt"] = (Json::Int64)now;
    summary["rev"] = (Json::UInt64)_rev;
    summary["overall"] = h["overall"];
    summary["score"] = eval.score;
    summary["components"] = components;
    summary["reasons"] = reasons;
    summary["alerts"] = h["alerts"];
    snap->summary = std::move(summary);
    snap->eval = std::move(eval);

    lock_guard<mutex> lck(_snap_mtx);
    _snapshot = snap;
}

void HealthMonitor::collectHost(HealthEvaluator &ev, Json::Value &summary, int64_t now) {
    using namespace HealthMonitorConfig;
    auto &gm = GlobalMonitor::Instance();
    Json::Value &host = summary["host"];

    auto cpu = gm.getCpuUsage();
    ev.addValue("host", "cpu.usage_pct", cpu.usagePct,
                globalThreshold(ResourceType::CPU, GlobalMonitorConfig::kCpuWarningThreshold, GlobalMonitorConfig::kCpuCriticalThreshold));
    host["cpu"]["cores"] = cpu.cores;
    host["cpu"]["usage_pct"] = num(cpu.usagePct);
    host["cpu"]["proc_usage_pct"] = num(cpu.procUsagePct);
    double load1 = NAN;
    if (HealthProbe::readLoadAvg(load1) && cpu.cores > 0) {
        double per_core = load1 / cpu.cores;
        ev.addValue("host", "cpu.load1_per_core", per_core, threshold("load1PerCore"));
        host["cpu"]["load1"] = num(load1);
        host["cpu"]["load1_per_core"] = num(per_core);
    } else {
        host["cpu"]["load1"] = Json::nullValue;
        host["cpu"]["load1_per_core"] = Json::nullValue;
    }

    auto mem = gm.getMemUsage();
    ev.addValue("host", "ram.usage_pct", mem.totalMemory > 0 ? (double)mem.usagePct : NAN,
                globalThreshold(ResourceType::MEMORY, GlobalMonitorConfig::kMemoryWarningThreshold, GlobalMonitorConfig::kMemoryCriticalThreshold));
    host["ram"]["total"] = (Json::UInt64)mem.totalMemory;
    host["ram"]["used"] = (Json::UInt64)mem.usageMemory;
    host["ram"]["usage_pct"] = num(mem.usagePct);
    host["ram"]["proc_rss"] = (Json::UInt64)mem.procUsageMemory;
    host["ram"]["proc_usage_pct"] = num(mem.procUsagePct);

    uint64_t swap_total = 0, swap_free = 0;
    if (HealthProbe::readSwap(swap_total, swap_free)) {
        double used_pct = swap_total > 0 ? 100.0 * (double)(swap_total - swap_free) / (double)swap_total : 0;
        ev.addValue("host", "swap.used_pct", used_pct, threshold("swapUsedPct"));
        host["swap"]["total"] = (Json::UInt64)swap_total;
        host["swap"]["used"] = (Json::UInt64)(swap_total - swap_free);
        host["swap"]["used_pct"] = num(used_pct);
    }

    host["nets"] = Json::arrayValue;
    int64_t now_ms = steadyNowMs();
    set<string> seen;
    for (const auto &n : gm.getNetUsage()) {
        if (n.name == "lo" || !n.is_up) {
            continue;
        }
        seen.insert(n.name);
        Json::Value nv;
        nv["name"] = n.name;
        nv["rx_mbps"] = num(n.rx_mbps);
        nv["tx_mbps"] = num(n.tx_mbps);
        nv["speed_mbps"] = num(n.speed_mbps);
        if (n.speed_mbps > 0) {
            double util = 100.0 * std::max(n.rx_mbps, n.tx_mbps) / n.speed_mbps;
            ev.addValue("host", "net.util_pct", util, threshold("netUtilPct"), n.name);
            nv["util_pct"] = num(util);
        } else {
            // Virtual NICs report no speed.
            nv["util_pct"] = Json::nullValue;
        }
        uint64_t errors = 0, drops = 0;
        nv["errors_per_sec"] = Json::nullValue;
        nv["drops_per_sec"] = Json::nullValue;
        if (HealthProbe::readNetErrorCounters(n.name, errors, drops)) {
            auto it = _net_counters.find(n.name);
            if (it != _net_counters.end() && now_ms > it->second.at_ms && errors >= it->second.errors
                && drops >= it->second.drops) {
                double sec = (now_ms - it->second.at_ms) / 1000.0;
                double err_rate = (errors - it->second.errors) / sec;
                double drop_rate = (drops - it->second.drops) / sec;
                ev.addValue("host", "net.errors_per_sec", err_rate, threshold("netErrorsPerSec"), n.name);
                ev.addValue("host", "net.drops_per_sec", drop_rate, threshold("netDropsPerSec"), n.name);
                nv["errors_per_sec"] = num(err_rate);
                nv["drops_per_sec"] = num(drop_rate);
            }
            NetCounters c;
            c.errors = errors;
            c.drops = drops;
            c.at_ms = now_ms;
            _net_counters[n.name] = c;
        }
        host["nets"].append(nv);
    }
    for (auto it = _net_counters.begin(); it != _net_counters.end();) {
        it = seen.count(it->first) ? std::next(it) : _net_counters.erase(it);
    }

    uint64_t fds = 0, fd_limit = 0;
    if (HealthProbe::readFdUsage(fds, fd_limit)) {
        host["fd"]["used"] = (Json::UInt64)fds;
        host["fd"]["limit"] = (Json::UInt64)fd_limit;
        if (fd_limit > 0) {
            double pct = 100.0 * (double)fds / (double)fd_limit;
            ev.addValue("host", "proc.fd_used_pct", pct, threshold("fdUsedPct"));
            host["fd"]["used_pct"] = num(pct);
        } else {
            host["fd"]["used_pct"] = Json::nullValue;
        }
    }
    host["threads"] = HealthProbe::readThreads();
    (void)now;
}

void HealthMonitor::collectProcess(HealthEvaluator &ev, Json::Value &summary, int64_t now) {
    using namespace HealthMonitorConfig;
    Json::Value &proc = summary["process"];
    proc["uptimeSec"] = (Json::Int64)(now - _started_at);
    proc["startedAt"] = (Json::Int64)_started_at;

    PollerDelay pollers, work_pollers;
    {
        lock_guard<mutex> lck(_delay_mtx);
        pollers = _pollers;
        work_pollers = _work_pollers;
    }
    int64_t now_ms = steadyNowMs();
    auto fill = [&](const char *group, const char *metric, const char *th_name, const PollerDelay &d) {
        double delay = effectiveDelayMs(d, now_ms);
        ev.addValue("process", metric, delay, threshold(th_name));
        Json::Value &g = proc[group];
        g["count"] = (Json::UInt64)d.count;
        g["delay_max_ms"] = num(delay);
        g["delay_avg_ms"] = d.valid ? num(d.delay_avg_ms) : Json::Value(Json::nullValue);
        g["load_max"] = d.valid ? Json::Value(d.load_max) : Json::Value(Json::nullValue);
        g["sampled_age_ms"] = d.valid ? Json::Value((Json::Int64)(now_ms - d.sampled_ms)) : Json::Value(Json::nullValue);
    };
    fill("pollers", "pollers.delay_max_ms", "pollerDelayMs", pollers);
    fill("work_pollers", "work_pollers.delay_max_ms", "workPollerDelayMs", work_pollers);
    ev.addValue("process", "pollers.load_max", pollers.valid ? (double)pollers.load_max : NAN, threshold("pollerLoadPct"));

    auto stats = BlockingExecutors::stats();
    proc["blocking"] = Json::arrayValue;
    for (const auto &st : stats) {
        Json::Value b;
        b["name"] = st.name;
        b["threads"] = st.threads;
        b["queued"] = (Json::UInt64)st.queued;
        b["running"] = st.running;
        b["completed"] = (Json::UInt64)st.completed;
        b["window_tasks"] = (Json::UInt64)st.window_tasks;
        b["wait_avg_ms"] = num(st.window_wait_avg_ms);
        b["wait_max_ms"] = num(st.window_wait_max_ms);
        b["busy_pct"] = num(st.window_busy_pct);
        proc["blocking"].append(b);
        if (st.threads > 0) {
            ev.addValue("process", "blocking.wait_max_ms", st.window_wait_max_ms, threshold("blockingWaitMs"), st.name);
            ev.addValue("process", "blocking.busy_pct", st.window_busy_pct, threshold("blockingBusyPct"), st.name);
        }
    }
    {
        lock_guard<mutex> lck(_snap_mtx);
        _blocking_stats = std::move(stats);
    }

    Json::Value &tc = summary["transcode"];
    tc["streams"] = (Json::UInt64)getTranscodeStreamCount();
    tc["decoders"] = (Json::UInt64)getVideoDecoderCount();
    tc["encoders"] = (Json::UInt64)getVideoEncoderCount();
    tc["motion"] = (Json::UInt64)getMotionProcessorCount();
    tc["mjpeg_ingest"] = (Json::UInt64)getMjpegIngestTranscodeCount();

    Json::Value &gpu = summary["gpu"];
    uint64_t fallbacks = getHwFallbackCount();
    if (fallbacks > _gpu_fallbacks) {
        _gpu_fallback_at = now;
    }
    _gpu_fallbacks = fallbacks;
    GET_CONFIG(int, fallback_window, kGpuFallbackWindowSec);
    bool recent = _gpu_fallback_at > 0 && now - _gpu_fallback_at < fallback_window;
    // A fallback is a single event: keep the warning for the window, without hysteresis.
    ev.addLevel("process", "gpu.fallbacks", recent ? HealthLevel::WARNING : HealthLevel::OK, (double)fallbacks, 0,
                std::string(), true);
    gpu["backends"] = getHwCodecBackends();
    gpu["load"] = getHwCodecLoad();
    gpu["decoders"] = (Json::UInt64)getHwDecoderCount();
    gpu["encoders"] = (Json::UInt64)getHwEncoderCount();
    gpu["fallbacks"] = (Json::UInt64)fallbacks;
    gpu["last_fallback_at"] = _gpu_fallback_at > 0 ? Json::Value((Json::Int64)_gpu_fallback_at) : Json::Value(Json::nullValue);
}

void HealthMonitor::collectStorage(HealthEvaluator &ev, Json::Value &summary, int64_t now) {
    auto &gm = GlobalMonitor::Instance();
    Json::Value &storage = summary["storage"];

    // Mounts the server writes (record path, DB, tier pools): other mounts (/boot, snaps) are
    // not the VMS's concern.
    set<string> mounts;
    storage["storage_write"] = Json::arrayValue;
    for (const auto &w : gm.getStorageWriteStatus()) {
        storage["storage_write"].append(w.toJson());
        if (!w.mount_point.empty()) {
            mounts.insert(w.mount_point);
        }
        HealthLevel level = !w.checked ? HealthLevel::UNKNOWN
                                       : (w.state == StorageWriteState::WRITABLE ? HealthLevel::OK : HealthLevel::CRITICAL);
        ev.addLevel("storage", "storage_write.writable", level, w.checked ? (double)w.state : NAN, 0, w.path, true);
    }

    const auto io = gm.getDiskIoSnapshot();
    auto hdd_th = globalThreshold(ResourceType::HDD, GlobalMonitorConfig::kHddWarningThreshold, GlobalMonitorConfig::kHddCriticalThreshold);
    storage["disks"] = Json::arrayValue;
    for (const auto &p : gm.getHddUsage()) {
        if (!mounts.count(p.mount_point)) {
            // HddMonitor alerts on every mount but has no recovery event: close it here.
            if (p.total_bytes > 0 && p.usage_pct < hdd_th.warn && AlertRegistry::Instance().isActive("DiskUsage", p.mount_point)) {
                Json::Value extra;
                extra["mountPoint"] = p.mount_point;
                extra["device"] = p.device;
                AlertRegistry::Instance().onEvent("DiskUsage", p.mount_point, "DiskRecovered", AlertRegistry::RECOVERED,
                                                  p.usage_pct, hdd_th.warn, extra, now);
            }
            continue;
        }
        Json::Value d;
        d["mount"] = p.mount_point;
        d["device"] = p.device;
        d["total_bytes"] = (Json::UInt64)p.total_bytes;
        d["used_bytes"] = (Json::UInt64)p.used_bytes;
        d["free_bytes"] = (Json::UInt64)p.free_bytes;
        d["usage_pct"] = num(p.usage_pct);
        d["read_only"] = p.read_only;
        auto disk_level = ev.addValue("storage", "disk.usage_pct", p.total_bytes > 0 ? (double)p.usage_pct : NAN, hdd_th, p.mount_point);
        // HddMonitor raises DiskWarning/DiskCritical per mount but has no recovery event.
        if (disk_level == HealthLevel::OK && AlertRegistry::Instance().isActive("DiskUsage", p.mount_point)) {
            Json::Value extra;
            extra["mountPoint"] = p.mount_point;
            extra["device"] = p.device;
            AlertRegistry::Instance().onEvent("DiskUsage", p.mount_point, "DiskRecovered", AlertRegistry::RECOVERED,
                                              p.usage_pct, hdd_th.warn, extra, now);
        }
        if (auto m = io.findMount(p.mount_point)) {
            if (m->valid) {
                d["io_util_pct"] = num(m->util_pct);
                d["await_ms"] = num(m->await_ms);
                d["read_mbps"] = num(m->read_mbps);
                d["write_mbps"] = num(m->write_mbps);
                d["io_state"] = diskIoLevelString(m->state);
                // DiskIoMonitor already sustained this level.
                ev.addLevel("storage", "disk_io.mount", fromDiskIoLevel(m->state), m->util_pct, 0, p.mount_point, true);
            }
        }
        storage["disks"].append(d);
    }
    Json::Value &dio = storage["disk_io"];
    dio["valid"] = io.valid;
    dio["iowait_pct"] = io.has_iowait ? num(io.iowait_pct) : Json::Value(Json::nullValue);
    dio["psi_io_full_pct"] = io.has_psi ? num(io.psi_full_pct) : Json::Value(Json::nullValue);
    dio["state"] = diskIoLevelString(io.system_state);
    if (io.valid) {
        ev.addLevel("storage", "disk_io.host", fromDiskIoLevel(io.system_state),
                    io.has_psi ? io.psi_full_pct : io.iowait_pct, 0, std::string(), true);
    }
    (void)now;
}

} // namespace managerkit

namespace managerkit {

void HealthMonitor::collectRecording(HealthEvaluator &ev, Json::Value &summary, int64_t now) {
    using namespace HealthMonitorConfig;
    Json::Value &rec = summary["recording"];
    const int stale_sec = recordStaleSec();
    map<string, RecordWrite> writes;
    {
        lock_guard<mutex> lck(_record_mtx);
        // Drop cameras gone for a day so the map does not grow with deleted cameras.
        for (auto it = _record_writes.begin(); it != _record_writes.end();) {
            it = now - it->second.last_write > 86400 ? _record_writes.erase(it) : std::next(it);
        }
        writes = _record_writes;
    }

    int total = 0, expected = 0, active = 0, stalled = 0, starting = 0;
    Json::Value stalled_list(Json::arrayValue);
    map<string, RecordingState> states;
    // Collect first: the device registry lock is not held while camera statistics are read.
    vector<pair<string, GenericRtspCameraImp::Ptr> > cameras;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
        auto impl = std::dynamic_pointer_cast<GenericRtspCameraImp>(src->getListener().lock());
        if (impl && impl->isEnabled()) {
            cameras.emplace_back(src->getDeviceTuple().device_id, impl);
        }
    });
    for (const auto &cam : cameras) {
        const string &camera_id = cam.first;
        const auto &impl = cam.second;
        auto stats = impl->getCameraStatisticImp();
        if (!stats) {
            continue;
        }
        ++total;
        auto expectation = impl->getRecordExpectation();
        int64_t last_write = 0;
        auto wit = writes.find(camera_id);
        if (wit != writes.end()) {
            last_write = wit->second.last_write;
        }
        auto state = evaluateRecording(expectation.expected, expectation.since, last_write, now, stale_sec);
        states[camera_id] = state;
        expected += expectation.expected ? 1 : 0;
        active += state == RecordingState::ACTIVE ? 1 : 0;
        starting += state == RecordingState::STARTING ? 1 : 0;

        auto prev_it = _recording_states.find(camera_id);
        bool was_stalled = prev_it != _recording_states.end() && prev_it->second == RecordingState::STALLED;
        if (state == RecordingState::STALLED || was_stalled) {
            auto params = stats->getParams();
            Json::Value extra;
            extra["cameraId"] = camera_id;
            extra["name"] = params.option.name;
            extra["ip"] = params.option.ip;
            extra["recordMode"] = getRecordModeString(static_cast<RecordMode>(expectation.mode));
            extra["lastWriteAt"] = last_write > 0 ? Json::Value((Json::Int64)last_write) : Json::Value(Json::nullValue);
            if (state == RecordingState::STALLED) {
                ++stalled;
                if (stalled_list.size() < 100) {
                    Json::Value c = extra;
                    c["expectedSince"] = (Json::Int64)expectation.since;
                    stalled_list.append(c);
                }
                if (!was_stalled) {
                    AlertRegistry::Instance().onEvent("RecordingStalled", camera_id, "RecordingStalled", AlertRegistry::CRITICAL,
                                                      last_write > 0 ? (double)(now - last_write) : NAN, stale_sec, extra, now);
                }
            } else {
                AlertRegistry::Instance().onEvent("RecordingStalled", camera_id, "RecordingRecovered", AlertRegistry::RECOVERED,
                                                  last_write > 0 ? (double)(now - last_write) : NAN, stale_sec, extra, now);
            }
        }
    }
    // Cameras removed or disabled while stalled: close their alert.
    for (const auto &kv : _recording_states) {
        if (kv.second == RecordingState::STALLED && !states.count(kv.first)) {
            Json::Value extra;
            extra["cameraId"] = kv.first;
            AlertRegistry::Instance().onEvent("RecordingStalled", kv.first, "RecordingRecovered", AlertRegistry::RECOVERED,
                                              NAN, stale_sec, extra, now);
        }
    }
    _recording_states.swap(states);

    double stalled_pct = expected > 0 ? 100.0 * stalled / expected : 0;
    ev.addValue("recording", "recording.stalled_pct", stalled_pct, threshold("recordStalledPct"));

    uint64_t write_errors = 0;
#ifdef ENABLE_MP4
    write_errors = mediakit::MP4Recorder::writeErrorCount();
#endif
    uint64_t new_errors = write_errors >= _write_errors ? write_errors - _write_errors : 0;
    _write_errors = write_errors;
    _write_error_cycles = new_errors > 0 ? _write_error_cycles + 1 : 0;
    // Errors are events: a cycle with new ones is a warning, three cycles in a row critical.
    HealthLevel err_level = _write_error_cycles >= 3 ? HealthLevel::CRITICAL
                          : (_write_error_cycles > 0 ? HealthLevel::WARNING : HealthLevel::OK);
    ev.addLevel("recording", "recording.write_errors", err_level, (double)new_errors, 0, std::string(), true);

    // The record directory must accept writes (the storage component shows every directory).
    for (const auto &w : GlobalMonitor::Instance().getStorageWriteStatus()) {
        if (!w.is_main_storage) {
            continue;
        }
        HealthLevel level = !w.checked ? HealthLevel::UNKNOWN
                                       : (w.state == StorageWriteState::WRITABLE ? HealthLevel::OK : HealthLevel::CRITICAL);
        ev.addLevel("recording", "recording.storage_writable", level, w.checked ? (double)w.state : NAN, 0, w.path, true);
    }

    rec["cameras"] = total;
    rec["expected"] = expected;
    rec["active"] = active;
    rec["starting"] = starting;
    rec["stalled"] = stalled;
    rec["stalled_pct"] = num(stalled_pct);
    rec["stale_sec"] = stale_sec;
    rec["write_errors"] = (Json::UInt64)write_errors;
    rec["write_errors_cycle"] = (Json::UInt64)new_errors;
    rec["stalled_cameras"] = stalled_list;
}

void HealthMonitor::collectDelivery(HealthEvaluator &ev, Json::Value &summary, int64_t now) {
    using namespace HealthMonitorConfig;
    Json::Value &del = summary["delivery"];
    auto &gm = GlobalMonitor::Instance();
    int live = 0, playback = 0;
    for (const auto &kv : gm.getReaderUsage()) {
        live += kv.second.first;
        playback += kv.second.second;
    }
    const int total = live + playback;
    del["viewers"]["live"] = live;
    del["viewers"]["playback"] = playback;
    del["viewers"]["total"] = total;
    // The server-wide reader limit comes from the license (-1 = unlimited).
    auto limit = gm.getThreshold(ResourceType::READER);
    if (limit.first > 0) {
        double pct = 100.0 * total / limit.first;
        ev.addValue("delivery", "viewers.limit_used_pct", pct, threshold("viewerLimitPct"));
        del["viewers"]["limit"] = limit.first;
        del["viewers"]["limit_used_pct"] = num(pct);
    } else {
        del["viewers"]["limit"] = Json::nullValue;
        del["viewers"]["limit_used_pct"] = Json::nullValue;
    }

    GET_CONFIG(int, window_sec, kPlayWindowSec);
    GET_CONFIG(int, min_requests, kPlayMinRequests);
    GET_CONFIG(float, interval_sec, kIntervalSec);
    PlayBucket bucket;
    Json::Value reasons(Json::objectValue);
    {
        lock_guard<mutex> lck(_play_mtx);
        bucket = _play_current;
        _play_current = PlayBucket();
        for (const auto &kv : _play_failure_reasons) {
            reasons[kv.first] = (Json::UInt64)kv.second;
        }
    }
    _play_buckets.push_back(bucket);
    size_t max_buckets = (size_t)std::max(1, (int)std::ceil(window_sec / std::max(1.0f, interval_sec)));
    while (_play_buckets.size() > max_buckets) {
        _play_buckets.pop_front();
    }
    uint64_t requests = 0, failures = 0;
    for (const auto &b : _play_buckets) {
        requests += b.requests;
        failures += b.failures;
    }
    double failure_pct = requests > 0 ? 100.0 * failures / requests : 0;
    if (requests >= (uint64_t)std::max(1, min_requests)) {
        ev.addValue("delivery", "play.failure_pct", failure_pct, threshold("playFailurePct"));
    } else {
        // Too few requests to judge: no evidence of a problem.
        ev.addLevel("delivery", "play.failure_pct", HealthLevel::OK, failure_pct, 0);
    }
    del["play"]["window_sec"] = window_sec;
    del["play"]["requests"] = (Json::UInt64)requests;
    del["play"]["failures"] = (Json::UInt64)failures;
    del["play"]["failure_pct"] = num(failure_pct);
    del["play"]["failures_by_reason"] = reasons;
    (void)now;
}

} // namespace managerkit
