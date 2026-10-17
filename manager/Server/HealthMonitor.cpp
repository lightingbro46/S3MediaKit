#include "HealthMonitor.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <set>
#include <thread>
#include "Common/config.h"
#include "Codec/TranscodeStats.h"
#include "Camera/GenericRtspCameraImp.h"
#include "Common/DeviceSource.h"
#include "Common/MediaSource.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Util/NoticeCenter.h"
#ifdef ENABLE_MP4
#include "Record/MP4Recorder.h"
#endif
#include "AlertRegistry.h"
#include "Thread/WorkThreadPool.h"
#include "Util/onceToken.h"
#include "Util/File.h"
#include "GlobalMonitor.h"
#include "HealthProbe.h"
#include "ClusterManager.h"
#include "Common/EventBus.h"
#include "Storage/DbStorage.h"
#ifdef ENABLE_SYNC_CHANGES
#include "Extension/SyncManager.h"
#endif
#ifdef ENABLE_TIER_STORAGE
#include "Local/TierStorageManager.h"
#endif
#include "Common/SerialTaskQueue.h"
#include "Local/TimeQuery.h"
#include "Storage/VmsResourceAssignment.h"

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
const string kBaselineWarmupSec = HM_FIELD "baselineWarmupSec";
const string kBaselineWindowSec = HM_FIELD "baselineWindowSec";
const string kSlowIntervalSec = HM_FIELD "slowIntervalSec";
const string kIntegrationWindowSec = HM_FIELD "integrationWindowSec";
const string kHistoryIntervalSec = HM_FIELD "historyIntervalSec";
const string kHistoryRetentionDays = HM_FIELD "historyRetentionDays";
const string kCoverageIntervalSec = HM_FIELD "coverageIntervalSec";
const string kRssGrowthWindowSec = HM_FIELD "rssGrowthWindowSec";

const vector<ThresholdDef> &thresholdDefs() {
    static const vector<ThresholdDef> defs = {
        // host (4.1)
        { "load1PerCore", "cpu.load1_per_core", 1.5, 3, true },
        { "swapUsedPct", "swap.used_pct", 20, 50, true },
        { "netUtilPct", "net.util_pct", 70, 90, true },
        { "netErrorsPerSec", "net.errors_per_sec", 0.01, 100, true },
        { "netDropsPerSec", "net.drops_per_sec", 10, 100, true },
        { "fdUsedPct", "proc.fd_used_pct", 70, 90, true },
        { "rssGrowthMbPerH", "ram.proc_rss_growth_mb_per_h", 200, 500, true },
        // process (4.2)
        { "pollerDelayMs", "pollers.delay_max_ms", 50, 200, true },
        { "pollerLoadPct", "pollers.load_max", 80, 95, true },
        { "workPollerDelayMs", "work_pollers.delay_max_ms", 100, 500, true },
        { "blockingWaitMs", "blocking.wait_max_ms", 1000, 5000, true },
        { "blockingBusyPct", "blocking.busy_pct", 80, 95, true },
        // restarts after a crash or kill (planned restarts end cleanly)
        { "restarts24h", "process.unclean_restarts_24h", 1, 3, true },
        // recording (4.4): any stalled camera is a warning, 5 % of the expected ones critical
        { "recordStalledPct", "recording.stalled_pct", 0.01, 5, true },
        // share of the cameras with expected time whose 24 h coverage is below coveragePct24hCrit
        { "recordCoverageLowPct", "recording.coverage_low_pct", 0.01, 5, true },
        // delivery (4.5)
        { "viewerLimitPct", "viewers.limit_used_pct", 80, 95, true },
        { "playFailurePct", "play.failure_pct", 5, 20, true },
        { "restoreFailed1h", "replay.restore_failures_1h", 1, 10, true },
        { "openLatencyP95Ms", "play.open_latency_p95_ms", 2000, 5000, true },
        // storage (4.6), database checks every monitor.slowIntervalSec
        { "dbWriteMs", "db.write_ms", 500, 2000, true },
        { "poolBacklogEtaSec", "pool.backlog_eta_sec", 21600, 86400, true },
        { "tierJobsFailed24h", "jobs.tiering_failed_24h", 1, 10, true },
        // cluster (4.7)
        { "syncLastOkAgeSec", "sync.last_ok_age_sec", 120, 600, true },
        { "webhookFailurePct", "webhook.failure_pct", 0.01, 20, true },
        // ingest (4.3), share of the enabled cameras
        { "cameraOfflinePct", "cameras.offline_pct", 5, 20, true },
        { "cameraNoVideoPct", "cameras.no_video_pct", 0.01, 5, true },
        // per camera stream (5.4)
        { "fpsRatio", "fps_ratio", 0.8, 0.5, false },
        { "bitrateRatio", "bitrate_ratio", 0.5, 0.2, false },
        { "lossPct", "loss_pct", 1, 5, true },
        { "lastFrameAgeMs", "last_frame_age_ms", 3000, 10000, true },
        { "disconnects1h", "disconnects_1h", 3, 10, true },
        { "uptimePct24h", "uptime_pct_24h", 99, 95, false },
        // per camera recording (H5)
        { "coveragePct24h", "coverage_pct_24h", 99, 95, false },
        { "recordGaps24h", "gaps_24h", 1, 6, true },
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
    mINI::Instance()[kBaselineWarmupSec] = 120;
    mINI::Instance()[kBaselineWindowSec] = 300;
    mINI::Instance()[kSlowIntervalSec] = 60;
    mINI::Instance()[kIntegrationWindowSec] = 300;
    mINI::Instance()[kHistoryIntervalSec] = 60;
    mINI::Instance()[kHistoryRetentionDays] = 30;
    mINI::Instance()[kCoverageIntervalSec] = 900;
    mINI::Instance()[kRssGrowthWindowSec] = 21600;
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

const vector<ThresholdPair> &thresholdPairs() {
    static const vector<ThresholdPair> pairs = []() {
        vector<ThresholdPair> out;
        for (const auto &def : thresholdDefs()) {
            out.push_back(ThresholdPair { warnKey(def), critKey(def), def.higher_is_worse });
        }
        for (const auto &pr : globalPairs()) {
            out.push_back(ThresholdPair { pr.first, pr.second, true });
        }
        return out;
    }();
    return pairs;
}

StreamBaselineConfig baselineConfig() {
    GET_CONFIG(int, warmup_sec, kBaselineWarmupSec);
    GET_CONFIG(int, window_sec, kBaselineWindowSec);
    StreamBaselineConfig cfg;
    cfg.warmup_sec = warmup_sec;
    // Statistic samples come every 60 s and a baseline needs 3 of them inside the window.
    cfg.window_sec = std::max(120, window_sec);
    return cfg;
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
        { kBaselineWarmupSec, 0 },
        { kBaselineWindowSec, 120 },
        { kSlowIntervalSec, 10 },
        { kIntegrationWindowSec, 10 },
        { kHistoryIntervalSec, 0 },
        { kHistoryRetentionDays, 1 },
        { kCoverageIntervalSec, 60 },
        { kRssGrowthWindowSec, 3600 },
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
            out.push_back(pr.warn_key);
            out.push_back(pr.crit_key);
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
        if (!changes.count(pr.warn_key) && !changes.count(pr.crit_key)) {
            continue;
        }
        double warn = 0, crit = 0;
        if (!parseNumber(value(pr.warn_key), warn) || !parseNumber(value(pr.crit_key), crit)) {
            return "not a number: " + pr.warn_key + "/" + pr.crit_key;
        }
        if (pr.higher_is_worse && warn >= crit) {
            return "warning must be lower than critical: " + pr.warn_key + " < " + pr.crit_key;
        }
        if (!pr.higher_is_worse && warn <= crit) {
            return "warning must be higher than critical: " + pr.warn_key + " > " + pr.crit_key;
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

CameraThresholds CameraThresholds::fromConfig() {
    using namespace HealthMonitorConfig;
    CameraThresholds th;
    th.fps_ratio = threshold("fpsRatio");
    th.bitrate_ratio = threshold("bitrateRatio");
    th.loss_pct = threshold("lossPct");
    th.last_frame_age_ms = threshold("lastFrameAgeMs");
    th.disconnects_1h = threshold("disconnects1h");
    th.uptime_pct_24h = threshold("uptimePct24h");
    th.coverage_pct = threshold("coveragePct24h");
    th.gaps = threshold("recordGaps24h");
    return th;
}

bool cameraOnline(const CameraInput &in) {
    return (in.streams[0].present && in.streams[0].live) || (in.streams[1].present && in.streams[1].live);
}

bool cameraNoVideo(const CameraInput &in, const CameraThresholds &th) {
    for (const auto &st : in.streams) {
        if (st.present && st.live && st.last_frame_age_ms >= th.last_frame_age_ms.crit) {
            return true;
        }
    }
    return false;
}

HealthLevel evaluateCamera(const CameraInput &in, const CameraThresholds &th, vector<string> &reasons) {
    HealthLevel level = HealthLevel::OK;
    auto raise = [&](HealthLevel l, const string &why) {
        level = worseLevel(level, l);
        reasons.push_back(why);
    };
    if (!cameraOnline(in)) {
        raise(HealthLevel::CRITICAL, "offline");
    }
    if (in.recording_stalled) {
        raise(HealthLevel::CRITICAL, "recording.stalled");
    }
    if (in.controller_expected && !in.controller_connected) {
        raise(HealthLevel::WARNING, "controller.disconnected");
    }
    // Coverage looks back 24 h: it warns, the live recording state above is what turns critical.
    auto bad = [](HealthLevel l) { return l == HealthLevel::WARNING || l == HealthLevel::CRITICAL; };
    if (in.coverage.pct >= 0 && bad(classifyValue(in.coverage.pct, th.coverage_pct))) {
        raise(HealthLevel::WARNING, "recording.coverage_pct_24h");
    }
    if (in.coverage.gaps >= 0 && bad(classifyValue(in.coverage.gaps, th.gaps))) {
        raise(HealthLevel::WARNING, "recording.gaps_24h");
    }
    static const char *names[2] = { "main", "sub" };
    for (int i = 0; i < 2; ++i) {
        const auto &st = in.streams[i];
        if (!st.present) {
            continue;
        }
        const string prefix = string(names[i]) + ".";
        if (!st.live) {
            // One of two streams down: the camera is still online.
            if (cameraOnline(in)) {
                raise(HealthLevel::WARNING, prefix + "offline");
            }
            continue;
        }
        auto age = classifyValue(st.last_frame_age_ms >= 0 ? st.last_frame_age_ms : NAN, th.last_frame_age_ms);
        if (age == HealthLevel::CRITICAL || age == HealthLevel::WARNING) {
            raise(age, prefix + (age == HealthLevel::CRITICAL ? "no_video" : "last_frame_age_ms"));
        }
        // Degraded quality only warns (5.4); a VBR camera's bitrate moves with the scene.
        auto warn_if = [&](double v, const HealthThreshold &t, const char *metric) {
            if (v < 0) {
                return;
            }
            auto l = classifyValue(v, t);
            if (l == HealthLevel::WARNING || l == HealthLevel::CRITICAL) {
                raise(HealthLevel::WARNING, prefix + metric);
            }
        };
        warn_if(st.fps_ratio, th.fps_ratio, "fps_ratio");
        warn_if(st.bitrate_ratio, th.bitrate_ratio, "bitrate_ratio");
        warn_if(st.loss_pct, th.loss_pct, "loss_pct");
        warn_if(st.disconnects_1h, th.disconnects_1h, "disconnects_1h");
        warn_if(st.uptime_pct_24h, th.uptime_pct_24h, "uptime_pct_24h");
    }
    return level;
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
    // rev keeps growing across restarts, so a rev the app kept from the previous run never
    // matches by chance and answers notModified.
    _rev = getCurrentMillisecond(true);
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
    _interval_sec = interval_sec;
#ifdef ENABLE_DEBUG
    if (const char *stall = getenv("S3MK_TEST_MONITOR_STALL_SEC")) {
        _test_stall_sec = atoi(stall);
        const char *at = getenv("S3MK_TEST_MONITOR_STALL_AT_CYCLE");
        _test_stall_cycle = at ? (uint64_t)std::max(1, atoi(at)) : 3;
        if (_test_stall_sec > 0) {
            WarnL << "TEST HOOK: monitor poller will stall " << _test_stall_sec << " s at cycle " << _test_stall_cycle;
        }
    }
#endif
    AlertRegistry::Instance().start();
    // History, restarts and baselines: SQLite, one task at a time on the API pool.
    _db_queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::api(), "health history");
    const int64_t started_at = _started_at;
    _db_queue->post([started_at]() {
        try {
            HealthMonitor::Instance()._process_start_id = ProcessStartImp().recordStart(started_at, std::string());
        } catch (const std::exception &ex) {
            WarnL << "Health monitor: recording the process start failed: " << ex.what();
        }
        map<string, StreamBaselineRow> stored;
        try {
            for (const auto &row : StreamBaselineImp().findAll()) {
                stored[row.camera_id + "/" + to_string(row.stream_type)] = row;
            }
        } catch (const std::exception &ex) {
            WarnL << "Health monitor: loading stream baselines failed: " << ex.what();
        }
        auto &self = HealthMonitor::Instance();
        lock_guard<mutex> lck(self._baseline_mtx);
        self._baseline_stored.swap(stored);
        self._baseline_loaded = true;
    });
#ifdef ENABLE_MP4
    // Only locks a mutex: it must not throw or block the listeners after it (tier, encryption).
    static int s_tag;
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        HealthMonitor::Instance().onRecordWritten(info.app, info.file_size);
    });
#endif
    _poller = monitorPoller();
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
    if (!_running.exchange(false)) {
        return;
    }
    _timer.reset();
    // A clean exit: the next start counts this run as ended on purpose (process.unclean_restarts_24h).
    const int64_t id = _process_start_id.load();
    if (id > 0) {
        try {
            ProcessStartImp().setStoppedAt(id, (int64_t)time(nullptr));
        } catch (const std::exception &ex) {
            WarnL << "Health monitor: recording the process stop failed: " << ex.what();
        }
    }
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

void HealthMonitor::onWebhookResult(bool ok) {
    lock_guard<mutex> lck(_webhook_mtx);
    ++_webhook_current.requests;
    if (!ok) {
        ++_webhook_current.failures;
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

void HealthMonitor::probeQueueDelay() {
    // Halfway to the next cycle, away from this one: how long a task waits on the monitor poller
    // while GlobalMonitor and the alert webhooks use it. One probe at a time.
    if (!_poller || _probe_due_ms > 0) {
        return;
    }
    const int64_t delay_ms = std::max<int64_t>(1, (int64_t)(_interval_sec.load() * 500));
    _probe_due_ms = steadyNowMs() + delay_ms;
    _poller->doDelayTask(delay_ms, []() -> uint64_t {
        auto &self = HealthMonitor::Instance();
        self._queue_delay_ms = std::max<int64_t>(0, steadyNowMs() - self._probe_due_ms);
        self._queue_delay_max_ms = std::max(self._queue_delay_max_ms, self._queue_delay_ms);
        self._probe_due_ms = 0;
        return 0;
    });
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

int64_t HealthMonitor::staleAfterSec(double interval_sec) {
    return std::max<int64_t>(30, (int64_t)std::ceil(3 * interval_sec));
}

void HealthMonitor::collectNow() {
    // The Timer re-arms from the moment it fired, so a start later than previous + interval is
    // time the monitor poller was busy elsewhere (or this cycle's own overrun).
    const int64_t cycle_start_ms = steadyNowMs();
    if (_last_cycle_start_ms > 0) {
        _lag_ms = std::max<int64_t>(0, cycle_start_ms - _last_cycle_start_ms - (int64_t)(_interval_sec.load() * 1000));
        _lag_max_ms = std::max(_lag_max_ms, _lag_ms);
    }
    _last_cycle_start_ms = cycle_start_ms;
#ifdef ENABLE_DEBUG
    if (_test_stall_sec > 0 && _cycle + 1 == _test_stall_cycle) {
        WarnL << "TEST HOOK: monitor poller stalls " << _test_stall_sec << " s";
        std::this_thread::sleep_for(std::chrono::seconds(_test_stall_sec));
    }
#endif
    int64_t now = (int64_t)time(nullptr);
    const auto collect_start = std::chrono::steady_clock::now();
    ++_cycle;
    _hysteresis.setCycle(_cycle);
    GET_CONFIG(int, sustain, HealthMonitorConfig::kSustainSamples);
    HealthEvaluator ev(_hysteresis, sustain, now);
    Json::Value summary(Json::objectValue);
    vector<Json::Value> cameras;

    try {
        collectHost(ev, summary, now);
        collectProcess(ev, summary, now);
        collectStorage(ev, summary, now);
        collectRecording(ev, summary, now);
        collectIngest(ev, summary, cameras, now);
        collectDelivery(ev, summary, now);
        collectCluster(ev, summary, now);
        evaluateSlow(ev, summary, now);
    } catch (const std::exception &ex) {
        WarnL << "Health monitor collect failed: " << ex.what();
    }
    // The answers are evaluated in a later cycle.
    requestPollerDelays();
    probeQueueDelay();
    startSlowCollect(now);
    startCoverage(now);

    auto eval = ev.finish();
    _hysteresis.sweep(_cycle);
    const double collect_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - collect_start).count();
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
    // Time on _poller for the collect and evaluate phase of this cycle (excludes this JSON assembly).
    _collect_max_ms = std::max(_collect_max_ms, collect_ms);
    summary["process"]["monitor"]["collect_ms"] = num(collect_ms);
    summary["process"]["monitor"]["collect_max_ms"] = num(_collect_max_ms);
    summary["process"]["monitor"]["cycle"] = (Json::UInt64)_cycle;
    Json::Value &mon = summary["process"]["monitor"];
    // This cycle is not finished while its JSON is built, so report the previous one.
    mon["total_ms_prev"] = _last_total_ms >= 0 ? num(_last_total_ms) : Json::Value(Json::nullValue);
    mon["total_max_ms"] = num(_total_max_ms);
    mon["lag_ms"] = _last_cycle_start_ms > 0 && _lag_ms >= 0 ? Json::Value((Json::Int64)_lag_ms) : Json::Value(Json::nullValue);
    mon["lag_max_ms"] = (Json::Int64)_lag_max_ms;
    int64_t queue_delay = _queue_delay_ms;
    if (_probe_due_ms > 0) {
        // Still not run: the time it is overdue is a lower bound of its delay.
        queue_delay = std::max(queue_delay, steadyNowMs() - _probe_due_ms);
    }
    mon["queue_delay_ms"] = queue_delay >= 0 ? Json::Value((Json::Int64)queue_delay) : Json::Value(Json::nullValue);
    mon["queue_delay_max_ms"] = (Json::Int64)std::max(_queue_delay_max_ms, queue_delay);
    recordHistory(h, summary, now);
    snap->summary = std::move(summary);
    snap->eval = std::move(eval);
    snap->cameras = std::move(cameras);

    {
        lock_guard<mutex> lck(_snap_mtx);
        _snapshot = snap;
    }
    _last_total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - collect_start).count();
    _total_max_ms = std::max(_total_max_ms, _last_total_ms);
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

// A retention-managed mount warns this far above its retention limit, critical twice as far.
static const double kRetentionMarginPct = 3.0;

map<string, double> HealthMonitor::retentionLimits() {
    map<string, double> limits;
    const auto snapshot = GlobalMonitor::Instance().getHddSnapshot(false);
#ifdef ENABLE_TIER_STORAGE
    // Tiering moves a file pool down once it passes its high watermark.
    vector<PoolView> pools;
    {
        lock_guard<mutex> lck(_slow_mtx);
        pools = _slow.pools;
    }
    for (const auto &pool : pools) {
        DiskPartition part;
        if (!pool.enabled || pool.high_watermark <= 0 || pool.mount_root.empty() ||
            !HddMonitor::findMountPoint(snapshot, pool.mount_root, part)) {
            continue;
        }
        auto &limit = limits[part.mount_point];
        limit = std::max(limit, (double)pool.high_watermark);
    }
#else
    // StorageManager deletes the oldest recordings once the record mount passes limitPercentUsage.
    GET_CONFIG(int, limit_percent, Storage::kLimitPercentUsage);
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    DiskPartition part;
    if (limit_percent > 0 && HddMonitor::findMountPoint(snapshot, File::absolutePath(app_name, mp4_save_path), part)) {
        limits[part.mount_point] = std::min(99, limit_percent);
    }
#endif
    return limits;
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
    const auto managed = retentionLimits();
    map<string, pair<double, double> > managed_th;
    storage["disks"] = Json::arrayValue;
    for (const auto &p : gm.getHddUsage()) {
        auto mit = managed.find(p.mount_point);
        HealthThreshold th = hdd_th;
        if (mit != managed.end()) {
            // Retention keeps this mount up to its limit: only above it by a margin is the
            // cleanup falling behind.
            th.warn = std::min(99.0, std::max(hdd_th.warn, mit->second + kRetentionMarginPct));
            th.crit = std::min(100.0, std::max(hdd_th.crit, mit->second + 2 * kRetentionMarginPct));
            th.crit = std::max(th.crit, std::min(100.0, th.warn + 1));
            managed_th[p.mount_point] = std::make_pair(th.warn, th.crit);
        }
        if (!mounts.count(p.mount_point)) {
            // HddMonitor alerts on every mount but has no recovery event: close it here.
            if (p.total_bytes > 0 && p.usage_pct < th.warn && AlertRegistry::Instance().isActive("DiskUsage", p.mount_point)) {
                Json::Value extra;
                extra["mountPoint"] = p.mount_point;
                extra["device"] = p.device;
                AlertRegistry::Instance().onEvent("DiskUsage", p.mount_point, "DiskRecovered", AlertRegistry::RECOVERED,
                                                  p.usage_pct, th.warn, extra, now);
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
        d["retention_limit_pct"] = mit != managed.end() ? num(mit->second) : Json::Value(Json::nullValue);
        d["warn_pct"] = num(th.warn);
        d["crit_pct"] = num(th.crit);
        auto disk_level = ev.addValue("storage", "disk.usage_pct", p.total_bytes > 0 ? (double)p.usage_pct : NAN, th, p.mount_point);
        // HddMonitor raises DiskWarning/DiskCritical per mount but has no recovery event.
        if (disk_level == HealthLevel::OK && AlertRegistry::Instance().isActive("DiskUsage", p.mount_point)) {
            Json::Value extra;
            extra["mountPoint"] = p.mount_point;
            extra["device"] = p.device;
            AlertRegistry::Instance().onEvent("DiskUsage", p.mount_point, "DiskRecovered", AlertRegistry::RECOVERED,
                                              p.usage_pct, th.warn, extra, now);
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
    AlertRegistry::Instance().setManagedDiskThresholds(managed_th);
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

    // Coverage of the last 24 h (computed every monitor.coverageIntervalSec on the API pool).
    map<string, CoverageResult> coverage;
    {
        lock_guard<mutex> lck(_coverage_mtx);
        coverage = _coverage;
    }
    GET_CONFIG(int, coverage_interval, kCoverageIntervalSec);
    const auto cov_th = threshold("coveragePct24h");
    int evaluated = 0, below_warn = 0, below_crit = 0;
    double pct_sum = 0;
    int64_t computed_at = 0;
    for (const auto &kv : coverage) {
        computed_at = std::max(computed_at, kv.second.at);
        // Less than an hour expected (schedule, new camera, just failed over): too little to judge.
        if (kv.second.coverage.expected_sec < 3600 || !_recording_states.count(kv.first)) {
            continue;
        }
        double pct = kv.second.coverage.pct();
        ++evaluated;
        pct_sum += pct;
        below_warn += pct <= cov_th.warn ? 1 : 0;
        below_crit += pct <= cov_th.crit ? 1 : 0;
    }
    const bool cov_fresh = computed_at > 0 && now - computed_at <= 3 * std::max(60, coverage_interval);
    double low_pct = evaluated > 0 ? 100.0 * below_crit / evaluated : 0;
    if (cov_fresh) {
        ev.addValue("recording", "recording.coverage_low_pct", low_pct, threshold("recordCoverageLowPct"));
    }
    Json::Value &cov = rec["coverage"];
    cov["computed_at"] = computed_at > 0 ? Json::Value((Json::Int64)computed_at) : Json::Value(Json::nullValue);
    cov["cameras_evaluated"] = evaluated;
    cov["below_warn"] = below_warn;
    cov["below_crit"] = below_crit;
    cov["low_pct"] = num(low_pct);
    cov["avg_pct"] = evaluated > 0 ? num(pct_sum / evaluated) : Json::Value(Json::nullValue);
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

    // Time to answer a player (MediaSource::findAsync): live streams that were found. A replay may wait
    // for an S3 restore; an unknown stream waits out General::kMaxStreamWaitTimeMS (a wrong id from a
    // client, not a slow server): both are only counted.
    vector<MediaSource::OpenSample> samples;
    MediaSource::takeOpenSamples(samples);
    OpenBucket ob;
    for (const auto &smp : samples) {
        if (!smp.found) {
            ++ob.not_found;
        } else if (smp.replay) {
            ++ob.replay_count;
            ob.replay_max_ms = std::max(ob.replay_max_ms, smp.ms);
        } else {
            ob.live_ms.push_back(smp.ms);
        }
    }
    _open_buckets.push_back(std::move(ob));
    while (_open_buckets.size() > max_buckets) {
        _open_buckets.pop_front();
    }
    vector<uint32_t> live_ms;
    uint32_t replay_count = 0, replay_max = 0, not_found = 0;
    for (const auto &b : _open_buckets) {
        live_ms.insert(live_ms.end(), b.live_ms.begin(), b.live_ms.end());
        replay_count += b.replay_count;
        replay_max = std::max(replay_max, b.replay_max_ms);
        not_found += b.not_found;
    }
    double p95 = percentileOf(live_ms, 95);
    if (live_ms.size() >= (size_t)std::max(1, min_requests)) {
        ev.addValue("delivery", "play.open_latency_p95_ms", p95, threshold("openLatencyP95Ms"));
    } else {
        ev.addLevel("delivery", "play.open_latency_p95_ms", HealthLevel::OK, std::isnan(p95) ? 0 : p95, 0);
    }
    Json::Value &open = del["play"]["open"];
    open["live_samples"] = (Json::UInt64)live_ms.size();
    open["live_p50_ms"] = num(percentileOf(live_ms, 50));
    open["live_p95_ms"] = num(p95);
    open["live_max_ms"] = num(percentileOf(live_ms, 100));
    open["replay_samples"] = replay_count;
    open["replay_max_ms"] = replay_count > 0 ? Json::Value(replay_max) : Json::Value(Json::nullValue);
    open["not_found"] = not_found;
    (void)now;
}

} // namespace managerkit

namespace managerkit {

static Json::Value optNum(double v) {
    return v < 0 ? Json::Value(Json::nullValue) : num(v);
}

void HealthMonitor::collectIngest(HealthEvaluator &ev, Json::Value &summary, vector<Json::Value> &cameras, int64_t now) {
    using namespace HealthMonitorConfig;
    GET_CONFIG(int, sustain, kSustainSamples);
    const auto th = CameraThresholds::fromConfig();
    const uint64_t now_ms = getCurrentMillisecond();

    vector<pair<string, GenericRtspCameraImp::Ptr> > list;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
        auto impl = std::dynamic_pointer_cast<GenericRtspCameraImp>(src->getListener().lock());
        if (impl && impl->isEnabled()) {
            list.emplace_back(src->getDeviceTuple().device_id, impl);
        }
    });

    int total = 0, online = 0, no_video = 0, warning = 0, critical = 0, unknown = 0;
    for (const auto &cam : list) {
        const string &camera_id = cam.first;
        auto stats = cam.second->getCameraStatisticImp();
        if (!stats) {
            continue;
        }
        ++total;
        auto params = stats->getParams();
        const auto &opt = params.option;

        CameraInput in;
        auto rs = _recording_states.find(camera_id);
        in.recording_stalled = rs != _recording_states.end() && rs->second == RecordingState::STALLED;
        in.controller_expected = !opt.manufacturer.empty() && opt.manufacturer != GENERIC_RTSP_CAMERA;
        in.controller_connected = params.device_stats.connect;

        Json::Value cj;
        cj["cameraId"] = camera_id;
        cj["name"] = opt.name;
        cj["ip"] = opt.ip;
        cj["manufacturer"] = opt.manufacturer;
        static const char *names[2] = { "main", "sub" };
        for (int type = 0; type < 2; ++type) {
            auto &st = in.streams[type];
            auto tuple_it = params.stream_map.find(type);
            bool disabled = type == PrimaryStream ? opt.disablePrimaryStream : opt.disableSecondaryStream;
            st.present = tuple_it != params.stream_map.end() && !tuple_it->second.empty() && !disabled;
            if (!st.present) {
                cj["streams"][names[type]] = Json::nullValue;
                continue;
            }
            auto info_it = params.sinfo_map.find(type);
            const StreamStatistic *sinfo = info_it != params.sinfo_map.end() ? &info_it->second : nullptr;
            st.live = sinfo && sinfo->live;

            StreamQualitySnapshot q;
            if (auto quality = cam.second->getStreamQuality(type)) {
                q = quality->snapshot(now);
                syncBaseline(camera_id, type, quality, q);
            }
            if (st.live) {
                auto media = MediaSource::find(RTSP_SCHEMA, tuple_it->second.vhost, tuple_it->second.device_id, tuple_it->second.stream_id);
                auto muxer = media ? media->getMuxer() : nullptr;
                uint64_t last = muxer ? muxer->lastVideoFrameMs() : 0;
                if (last > 0) {
                    st.last_frame_age_ms = now_ms > last ? (double)(now_ms - last) : 0;
                }
            }
            if (q.fps > 0 && q.fps_baseline > 0) {
                st.fps_ratio = q.fps / q.fps_baseline;
            }
            if (q.bitrate_kbps > 0 && q.bitrate_baseline > 0) {
                st.bitrate_ratio = q.bitrate_kbps / q.bitrate_baseline;
            }
            st.loss_pct = st.live ? q.loss_pct : -1;
            st.disconnects_1h = q.disconnects_1h;
            // Over a short window one reconnect already looks like a bad day: judge from 1 h on.
            st.uptime_pct_24h = now - q.window_started_at >= 3600 ? q.uptime_pct_24h : -1;

            Json::Value sj;
            sj["streamId"] = tuple_it->second.stream_id;
            sj["live"] = st.live;
            sj["status"] = sinfo ? sinfo->status : "";
            sj["lastChangeAt"] = sinfo ? (Json::Int64)sinfo->last_change_status : 0;
            sj["codec"] = sinfo ? sinfo->vcodec : "";
            sj["width"] = sinfo ? sinfo->width : 0;
            sj["height"] = sinfo ? sinfo->height : 0;
            sj["fps"] = optNum(q.fps);
            sj["fps_baseline"] = optNum(q.fps_baseline);
            sj["fps_ratio"] = optNum(st.fps_ratio);
            sj["bitrate_kbps"] = optNum(q.bitrate_kbps);
            sj["bitrate_baseline"] = optNum(q.bitrate_baseline);
            sj["bitrate_ratio"] = optNum(st.bitrate_ratio);
            sj["baseline_at"] = q.baseline_at > 0 ? Json::Value((Json::Int64)q.baseline_at) : Json::Value(Json::nullValue);
            sj["sampled_at"] = q.sampled_at > 0 ? Json::Value((Json::Int64)q.sampled_at) : Json::Value(Json::nullValue);
            sj["loss_pct"] = optNum(st.loss_pct);
            sj["last_frame_age_ms"] = optNum(st.last_frame_age_ms);
            sj["disconnects_1h"] = q.disconnects_1h;
            sj["disconnects_24h"] = q.disconnects_24h;
            sj["uptime_pct_24h"] = optNum(q.uptime_pct_24h);
            sj["window_started_at"] = (Json::Int64)q.window_started_at;
            cj["streams"][names[type]] = sj;
        }

        {
            lock_guard<mutex> lck(_coverage_mtx);
            auto cit = _coverage.find(camera_id);
            if (cit != _coverage.end() && cit->second.coverage.expected_sec > 0) {
                const auto &c = cit->second.coverage;
                cj["recording"]["coverage_pct_24h"] = num(c.pct());
                cj["recording"]["expected_sec_24h"] = (Json::Int64)c.expected_sec;
                cj["recording"]["gaps_24h"] = c.gaps;
                cj["recording"]["longest_gap_sec"] = (Json::Int64)c.longest_gap_sec;
                cj["recording"]["coverage_at"] = (Json::Int64)cit->second.at;
                // Same rule as the component: at least an hour expected.
                if (c.expected_sec >= 3600) {
                    in.coverage.pct = c.pct();
                    in.coverage.gaps = c.gaps;
                }
            } else {
                cj["recording"]["coverage_pct_24h"] = Json::nullValue;
                cj["recording"]["gaps_24h"] = Json::nullValue;
            }
        }

        vector<string> reasons;
        auto observed = evaluateCamera(in, th, reasons);
        int64_t since = now;
        // The same hysteresis as the server metrics: a camera does not flip per sample.
        auto level = _hysteresis.update("camera/" + camera_id, observed, sustain, now, &since);
        bool is_online = cameraOnline(in);
        bool is_no_video = is_online && cameraNoVideo(in, th);
        online += is_online ? 1 : 0;
        no_video += is_no_video ? 1 : 0;
        warning += level == HealthLevel::WARNING ? 1 : 0;
        critical += level == HealthLevel::CRITICAL ? 1 : 0;
        unknown += level == HealthLevel::UNKNOWN ? 1 : 0;

        cj["health"] = healthLevelString(level);
        cj["healthSince"] = (Json::Int64)since;
        cj["observed"] = healthLevelString(observed);
        cj["reasons"] = Json::arrayValue;
        for (const auto &r : reasons) {
            cj["reasons"].append(r);
        }
        cj["online"] = is_online;
        cj["recording"]["state"] = rs != _recording_states.end() ? recordingStateString(rs->second) : "not_expected";
        auto expectation = cam.second->getRecordExpectation();
        cj["recording"]["expected"] = expectation.expected;
        cj["recording"]["mode"] = getRecordModeString(static_cast<RecordMode>(expectation.mode));
        {
            lock_guard<mutex> lck(_record_mtx);
            auto wit = _record_writes.find(camera_id);
            cj["recording"]["lastWriteAt"] = wit != _record_writes.end() ? Json::Value((Json::Int64)wit->second.last_write)
                                                                          : Json::Value(Json::nullValue);
        }
        if (in.controller_expected) {
            cj["controller"]["connected"] = params.device_stats.connect;
            cj["controller"]["status"] = params.device_stats.status;
        } else {
            cj["controller"] = Json::nullValue;
        }
        cameras.push_back(std::move(cj));
    }

    // Most severe first, then by name.
    auto rank = [](const Json::Value &c) {
        const string h = c["health"].asString();
        return h == "critical" ? 0 : (h == "warning" ? 1 : (h == "unknown" ? 2 : 3));
    };
    std::stable_sort(cameras.begin(), cameras.end(), [&](const Json::Value &a, const Json::Value &b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) {
            return ra < rb;
        }
        return a["name"].asString() < b["name"].asString();
    });

    double offline_pct = total > 0 ? 100.0 * (total - online) / total : 0;
    double no_video_pct = total > 0 ? 100.0 * no_video / total : 0;
    ev.addValue("ingest", "cameras.offline_pct", offline_pct, threshold("cameraOfflinePct"));
    ev.addValue("ingest", "cameras.no_video_pct", no_video_pct, threshold("cameraNoVideoPct"));

    Json::Value &ing = summary["ingest"];
    ing["cameras"] = total;
    ing["online"] = online;
    ing["offline"] = total - online;
    ing["offline_pct"] = num(offline_pct);
    ing["no_video"] = no_video;
    ing["no_video_pct"] = num(no_video_pct);
    ing["health"]["critical"] = critical;
    ing["health"]["warning"] = warning;
    ing["health"]["unknown"] = unknown;
    ing["health"]["ok"] = total - critical - warning - unknown;
}

} // namespace managerkit

namespace managerkit {

HealthLevel poolHealthLevel(const string &health_status, bool stale) {
    if (stale) {
        return HealthLevel::UNKNOWN;
    }
    if (health_status == "OFFLINE" || health_status == "CRITICAL") {
        return HealthLevel::CRITICAL;
    }
    if (health_status == "HIGH" || health_status == "WARNING") {
        return HealthLevel::WARNING;
    }
    return health_status == "OK" ? HealthLevel::OK : HealthLevel::UNKNOWN;
}

HealthLevel failureRateLevel(uint64_t requests, uint64_t failures, uint64_t min_requests,
                             const HealthThreshold &th, double &failure_pct) {
    failure_pct = requests > 0 ? 100.0 * (double)failures / (double)requests : 0;
    if (failures == 0) {
        return HealthLevel::OK;
    }
    if (requests < min_requests) {
        return HealthLevel::WARNING;
    }
    return classifyValue(failure_pct, th);
}

// Keeps one bucket per cycle over `window_sec`, returns the totals.
template <typename Bucket>
static Bucket rollBuckets(std::deque<Bucket> &buckets, const Bucket &current, int window_sec, float interval_sec) {
    buckets.push_back(current);
    size_t max_buckets = (size_t)std::max(1, (int)std::ceil(window_sec / std::max(1.0f, interval_sec)));
    while (buckets.size() > max_buckets) {
        buckets.pop_front();
    }
    Bucket total;
    for (const auto &b : buckets) {
        total.requests += b.requests;
        total.failures += b.failures;
    }
    return total;
}

void HealthMonitor::collectCluster(HealthEvaluator &ev, Json::Value &summary, int64_t now) {
    using namespace HealthMonitorConfig;
    Json::Value &cl = summary["cluster"];
    GET_CONFIG(string, self_id, General::kMediaServerId);
    GET_CONFIG(int, window_sec, kIntegrationWindowSec);
    GET_CONFIG(float, interval_sec, kIntervalSec);

    // Peers answer ClusterManager's health check (every 60 s); only reachable ones have a URL.
    auto &cm = ClusterManager::Instance();
    auto reachable = cm.getPeerUrls();
    int known = 0, unreachable = 0;
    Json::Value peers(Json::arrayValue);
    for (const auto &id : cm.getMediaServerIds()) {
        if (id == self_id) {
            continue;
        }
        ++known;
        bool ok = reachable.count(id) > 0;
        unreachable += ok ? 0 : 1;
        Json::Value pj;
        pj["id"] = id;
        pj["name"] = cm.getMediaServer(id).name;
        pj["reachable"] = ok;
        peers.append(pj);
    }
    // A peer down does not stop this node from serving its own cameras: warning only.
    ev.addLevel("cluster", "cluster.peers_unreachable", unreachable > 0 ? HealthLevel::WARNING : HealthLevel::OK,
                unreachable, 0);
    cl["peers"] = peers;
    cl["peers_known"] = known;
    cl["peers_unreachable"] = unreachable;

    Json::Value &sync = cl["sync"];
#ifdef ENABLE_SYNC_CHANGES
    auto sv = SyncManager::Instance().healthView();
    sync["enabled"] = sv.running;
    if (sv.running) {
        sync["bootstrapped"] = sv.bootstrapped;
        sync["single_node"] = sv.single_node;
        sync["decommissioned"] = sv.decommissioned;
        // Removed from the cluster by the service API: this node's changes no longer reach the others.
        ev.addLevel("cluster", "cluster.decommissioned", sv.decommissioned ? HealthLevel::CRITICAL : HealthLevel::OK,
                    sv.decommissioned ? 1 : 0, 0, std::string(), true);
        sync["peers"] = Json::arrayValue;
        map<string, uint64_t> seen;
        if (!sv.single_node && !sv.decommissioned) {
            ev.addLevel("cluster", "sync.bootstrapped", sv.bootstrapped ? HealthLevel::OK : HealthLevel::WARNING,
                        sv.bootstrapped ? 1 : 0, 0);
            const uint64_t now_ms = getCurrentMillisecond(true);
            const auto th = threshold("syncLastOkAgeSec");
            for (const auto &kv : sv.last_complete_pull_ms) {
                auto it = _sync_peer_seen_ms.find(kv.first);
                uint64_t first_seen = it != _sync_peer_seen_ms.end() ? it->second : now_ms;
                seen[kv.first] = first_seen;
                // Until its first complete pull a peer is measured from when it joined.
                uint64_t ref = std::max(kv.second, first_seen);
                double age = now_ms > ref ? (now_ms - ref) / 1000.0 : 0;
                ev.addValue("cluster", "sync.last_ok_age_sec", age, th, kv.first);
                Json::Value pj;
                pj["id"] = kv.first;
                pj["last_ok_at"] = kv.second > 0 ? Json::Value((Json::Int64)(kv.second / 1000)) : Json::Value(Json::nullValue);
                pj["last_ok_age_sec"] = num(age);
                sync["peers"].append(pj);
            }
        }
        _sync_peer_seen_ms.swap(seen);
    }
#else
    sync["enabled"] = false;
#endif

    // Webhooks to the VMS/ESC, counted after their retries.
    PlayBucket current;
    {
        lock_guard<mutex> lck(_webhook_mtx);
        current = _webhook_current;
        _webhook_current = PlayBucket();
    }
    auto total = rollBuckets(_webhook_buckets, current, window_sec, interval_sec);
    double failure_pct = 0;
    const auto wh_th = threshold("webhookFailurePct");
    auto wh_level = failureRateLevel(total.requests, total.failures, 10, wh_th, failure_pct);
    ev.addLevel("cluster", "webhook.failure_pct", wh_level, failure_pct,
                wh_level == HealthLevel::CRITICAL ? wh_th.crit : wh_th.warn);
    cl["webhook"]["window_sec"] = window_sec;
    cl["webhook"]["requests"] = (Json::UInt64)total.requests;
    cl["webhook"]["failures"] = (Json::UInt64)total.failures;
    cl["webhook"]["failure_pct"] = num(failure_pct);

    // Device events to Kafka (EventBus is built without ENABLE_KAFKA too, then never enabled).
    Json::Value &kj = cl["kafka"];
    auto &bus = EventBus::Instance();
    kj["enabled"] = bus.enabled();
    if (bus.enabled()) {
        auto st = bus.stats();
        uint64_t dropped = st.droppedSpool + st.droppedMem;
        if (dropped > _kafka_dropped) {
            _kafka_dropped_at = now;
        }
        _kafka_dropped = dropped;
        bool recent_drop = _kafka_dropped_at > 0 && now - _kafka_dropped_at < window_sec;
        // Lost events are single occurrences: keep the warning for the window, without hysteresis.
        ev.addLevel("cluster", "kafka.dropped", recent_drop ? HealthLevel::WARNING : HealthLevel::OK, (double)dropped, 0,
                    std::string(), true);
        // SPOOL: no broker takes the events, they wait on disk.
        ev.addLevel("cluster", "kafka.spool", st.mode == EventBus::Mode::Spool ? HealthLevel::WARNING : HealthLevel::OK,
                    (double)st.spoolPending, 0);
        kj["mode"] = EventBus::modeName(st.mode);
        kj["brokers_up"] = st.brokersUp;
        kj["brokers_known"] = st.brokersKnown;
        kj["queued"] = st.queued;
        kj["delivered"] = (Json::UInt64)st.delivered;
        kj["spool_pending"] = (Json::UInt64)st.spoolPending;
        kj["spool_bytes"] = (Json::UInt64)st.spoolBytes;
        kj["dropped_rate"] = (Json::UInt64)st.droppedRate;
        kj["dropped"] = (Json::UInt64)dropped;
        kj["last_drop_at"] = _kafka_dropped_at > 0 ? Json::Value((Json::Int64)_kafka_dropped_at) : Json::Value(Json::nullValue);
    }
}

void HealthMonitor::startSlowCollect(int64_t now) {
    GET_CONFIG(int, slow_interval, HealthMonitorConfig::kSlowIntervalSec);
    if (_slow_started_at > 0 && now - _slow_started_at < std::max(10, slow_interval)) {
        return;
    }
    // The previous run still waits on the database: do not queue another one behind it.
    if (_slow_running.exchange(true)) {
        return;
    }
    _slow_started_at = now;
    BlockingExecutors::api()->async([]() {
        auto &self = HealthMonitor::Instance();
        try {
            self.slowCollect();
        } catch (const std::exception &ex) {
            WarnL << "Health monitor database checks failed: " << ex.what();
        }
        self._slow_running = false;
    });
}

void HealthMonitor::slowCollect() {
    SlowResult r;
    r.valid = true;
    r.at = (int64_t)time(nullptr);
    try {
        // Time to take the write lock of the MediaServer database: every writer waits this long.
        SqliteQueryExecutor executor(Database::kMediaServerDb);
        int64_t begin_ms = steadyNowMs();
        auto txn = executor.beginWriteTxn();
        txn->commit();
        r.db_write_ms = (double)(steadyNowMs() - begin_ms);
    } catch (const std::exception &ex) {
        r.db_write_ms = -1;
        r.db_error = ex.what();
    }
    try {
        ProcessStartImp().countRestartsSince(r.at - 86400, r.restarts_24h, r.unclean_restarts_24h);
        r.restarts_known = true;
    } catch (const std::exception &ex) {
        WarnL << "Health monitor: counting restarts failed: " << ex.what();
    }
    try {
        // Caches and pools fill up during the first half hour: that growth is not a leak.
        GET_CONFIG(int, window_sec, HealthMonitorConfig::kRssGrowthWindowSec);
        auto samples = HealthMetricsImp().findRssSince(std::max(r.at - window_sec, _started_at + 1800));
        if (!samples.empty()) {
            r.rss_span_sec = samples.back().first - samples.front().first;
        }
        r.rss_growth_mb_per_h = rssGrowthMbPerHour(samples, 7200);
    } catch (const std::exception &ex) {
        WarnL << "Health monitor: reading the RSS history failed: " << ex.what();
    }
#ifdef ENABLE_TIER_STORAGE
    r.tier = true;
    auto &tsm = TierStorageManager::Instance();
    for (const auto &p : tsm.listPools()) {
        PoolView v;
        v.id = p.id;
        v.name = p.name;
        v.tier = p.tier;
        v.type = p.type;
        v.enabled = p.enabled != 0;
        v.health_check = p.health_check_enabled != 0;
        v.usage_pct = p.usage_pct;
        if (p.type == "LOCAL_DISK" || p.type == "NAS") {
            v.mount_root = TierStorageManager::resolveFilePoolMountRoot(p);
            v.high_watermark = p.high_watermark_percent;
        }
        PoolHealthInfo health;
        if (tsm.getPoolHealth(p.id, health)) {
            v.health_known = true;
            v.health_status = health.health_status;
            v.stale = health.stale;
            v.message = health.message;
        }
        PoolThroughputStats throughput;
        if (tsm.getPoolThroughput(p.id, throughput)) {
            v.throughput_known = true;
            v.throughput_low = throughput.low;
            v.write_mbps = throughput.write_mbps;
            v.backlog_bytes = throughput.backlog_bytes;
            v.backlog_eta_sec = throughput.backlog_eta_sec;
        }
        r.pools.push_back(v);
    }
    const string failed = jobStatusToString(JobStatus::FAILED);
    r.tiering_failed_24h = tsm.countTieringJobs("", failed, r.at - 86400);
    r.tiering_pending = tsm.countTieringJobs("", jobStatusToString(JobStatus::PENDING));
    r.tiering_running = tsm.countTieringJobs("", jobStatusToString(JobStatus::RUNNING));
    r.restore_failed_24h = tsm.countRestoreJobs("", failed, r.at - 86400);
    r.restore_failed_1h = tsm.countRestoreJobs("", failed, r.at - 3600);
    r.restore_pending = tsm.countRestoreJobs("", jobStatusToString(JobStatus::PENDING));
#endif
    lock_guard<mutex> lck(_slow_mtx);
    _slow = std::move(r);
}

void HealthMonitor::evaluateSlow(HealthEvaluator &ev, Json::Value &summary, int64_t now) {
    using namespace HealthMonitorConfig;
    SlowResult r;
    {
        lock_guard<mutex> lck(_slow_mtx);
        r = _slow;
    }
    GET_CONFIG(int, slow_interval, kSlowIntervalSec);
    Json::Value &storage = summary["storage"];
    Json::Value &db = storage["database"];
    Json::Value &tier = storage["tier"];
#ifdef ENABLE_TIER_STORAGE
    tier["enabled"] = true;
#else
    tier["enabled"] = false;
#endif
    if (!r.valid) {
        db["checked_at"] = Json::nullValue;
        db["write_ms"] = Json::nullValue;
        return;
    }
    // A run stuck behind a busy API pool leaves old numbers: shown, but not evaluated.
    const bool fresh = now - r.at <= 3 * std::max(10, slow_interval);
    db["checked_at"] = (Json::Int64)r.at;
    db["stale"] = !fresh;
    db["write_ms"] = r.db_write_ms >= 0 ? num(r.db_write_ms) : Json::Value(Json::nullValue);
    db["error"] = r.db_error;
    if (fresh) {
        if (r.db_write_ms < 0) {
            // BEGIN IMMEDIATE failed after busy_timeout (locked) or the file cannot be written.
            ev.addLevel("storage", "db.write_ms", HealthLevel::CRITICAL, NAN, 0, std::string(), true);
        } else {
            ev.addValue("storage", "db.write_ms", r.db_write_ms, threshold("dbWriteMs"));
        }
    }
    Json::Value &ram = summary["host"]["ram"];
    ram["proc_rss_growth_mb_per_h"] = std::isnan(r.rss_growth_mb_per_h) ? Json::Value(Json::nullValue) : num(r.rss_growth_mb_per_h);
    ram["proc_rss_growth_span_sec"] = (Json::Int64)r.rss_span_sec;
    Json::Value &proc = summary["process"];
    proc["restarts_24h"] = r.restarts_known ? Json::Value(r.restarts_24h) : Json::Value(Json::nullValue);
    proc["unclean_restarts_24h"] = r.restarts_known ? Json::Value(r.unclean_restarts_24h) : Json::Value(Json::nullValue);
    if (fresh) {
        // Needs 2 h of samples after the first half hour; until then nothing is reported.
        if (!std::isnan(r.rss_growth_mb_per_h)) {
            ev.addValue("host", "ram.proc_rss_growth_mb_per_h", r.rss_growth_mb_per_h, threshold("rssGrowthMbPerH"));
        }
        if (r.restarts_known) {
            ev.addValue("process", "process.unclean_restarts_24h", r.unclean_restarts_24h, threshold("restarts24h"));
        }
    }
    if (!r.tier) {
        return;
    }

    tier["pools"] = Json::arrayValue;
    const auto eta_th = threshold("poolBacklogEtaSec");
    for (const auto &p : r.pools) {
        Json::Value pj;
        pj["id"] = p.id;
        pj["name"] = p.name;
        pj["tier"] = p.tier;
        pj["type"] = p.type;
        pj["enabled"] = p.enabled;
        pj["usage_pct"] = num(p.usage_pct);
        pj["health_check"] = p.health_check;
        pj["health_status"] = p.health_known ? Json::Value(p.health_status) : Json::Value(Json::nullValue);
        pj["health_stale"] = p.stale;
        pj["message"] = p.message;
        pj["throughput_low"] = p.throughput_known ? Json::Value(p.throughput_low) : Json::Value(Json::nullValue);
        pj["write_mbps"] = p.throughput_known ? num(p.write_mbps) : Json::Value(Json::nullValue);
        pj["backlog_bytes"] = p.throughput_known ? Json::Value((Json::Int64)p.backlog_bytes) : Json::Value(Json::nullValue);
        pj["backlog_eta_sec"] = p.throughput_known && p.backlog_eta_sec >= 0 ? Json::Value((Json::Int64)p.backlog_eta_sec)
                                                                              : Json::Value(Json::nullValue);
        tier["pools"].append(pj);
        if (!fresh || !p.enabled) {
            continue;
        }
        const string &detail = p.name.empty() ? p.id : p.name;
        if (p.health_check && p.health_known) {
            // The pool health check has its own failure/recovery thresholds.
            ev.addLevel("storage", "pool.health", poolHealthLevel(p.health_status, p.stale), p.usage_pct, 0, detail, true);
        }
        if (p.throughput_known) {
            // PoolThroughputTracker already sustains "low".
            ev.addLevel("storage", "pool.throughput_low", p.throughput_low ? HealthLevel::WARNING : HealthLevel::OK,
                        p.write_mbps, 0, detail, true);
            if (p.backlog_eta_sec >= 0) {
                ev.addValue("storage", "pool.backlog_eta_sec", (double)p.backlog_eta_sec, eta_th, detail);
            }
        }
    }
    Json::Value &jobs = tier["jobs"];
    jobs["tiering_failed_24h"] = r.tiering_failed_24h;
    jobs["tiering_pending"] = r.tiering_pending;
    jobs["tiering_running"] = r.tiering_running;
    jobs["restore_failed_24h"] = r.restore_failed_24h;
    jobs["restore_failed_1h"] = r.restore_failed_1h;
    jobs["restore_pending"] = r.restore_pending;
    summary["delivery"]["restore"]["failed_1h"] = r.restore_failed_1h;
    summary["delivery"]["restore"]["failed_24h"] = r.restore_failed_24h;
    if (fresh) {
        ev.addValue("storage", "jobs.tiering_failed_24h", r.tiering_failed_24h, threshold("tierJobsFailed24h"));
        // A replay waiting on a restore that failed: viewers see no video.
        ev.addValue("delivery", "replay.restore_failures_1h", r.restore_failed_1h, threshold("restoreFailed1h"));
    }
}

} // namespace managerkit

// ──────────────────────────────────────────────────────────────────
// History, coverage, restarts, RSS growth, open latency, baselines (H5)
// ──────────────────────────────────────────────────────────────────
namespace managerkit {

vector<RecordMode> recordModesByHour(const string &record_schedules) {
    vector<RecordMode> modes(7 * 24, RecordMode::NoRecord);
    for (const auto &kv : RecordScheduler::parseRecordScheduleStr(record_schedules)) {
        const auto &item = kv.second;
        if (item.day >= 0 && item.day < 7 && item.hour >= 0 && item.hour < 24) {
            modes[(size_t)(item.day * 24 + item.hour)] = item.mode;
        }
    }
    return modes;
}

vector<TimeSpan> scheduledRecordSpans(bool enable_record, const vector<RecordMode> &modes_by_hour, int64_t from, int64_t to) {
    vector<TimeSpan> out;
    if (!enable_record || modes_by_hour.size() < 7 * 24 || from >= to) {
        return out;
    }
    int64_t t = from;
    while (t < to) {
        time_t tt = (time_t)t;
        struct tm lt;
        localtime_r(&tt, &lt);
        // End of this local hour (a DST change moves the clock by whole hours).
        int64_t end = std::min(to, t + 3600 - (lt.tm_min * 60 + lt.tm_sec));
        RecordMode mode = modes_by_hour[(size_t)RecordScheduler::getRecordScheduledActive(tt)];
        if (mode == RecordMode::RecordAlways || mode == RecordMode::RecordLowResAndMotion) {
            if (!out.empty() && out.back().second == t) {
                out.back().second = end;
            } else {
                out.emplace_back(t, end);
            }
        }
        t = end;
    }
    return out;
}

vector<TimeSpan> mergeSpans(vector<TimeSpan> spans) {
    std::sort(spans.begin(), spans.end());
    vector<TimeSpan> out;
    for (const auto &s : spans) {
        if (s.second <= s.first) {
            continue;
        }
        if (!out.empty() && s.first <= out.back().second) {
            out.back().second = std::max(out.back().second, s.second);
        } else {
            out.push_back(s);
        }
    }
    return out;
}

vector<TimeSpan> intersectSpans(const vector<TimeSpan> &a, const vector<TimeSpan> &b) {
    vector<TimeSpan> out;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        int64_t start = std::max(a[i].first, b[j].first);
        int64_t end = std::min(a[i].second, b[j].second);
        if (start < end) {
            out.emplace_back(start, end);
        }
        if (a[i].second < b[j].second) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;
}

RecordCoverage computeRecordCoverage(const vector<TimeSpan> &expected, const vector<TimeSpan> &recorded, int64_t gap_min_sec) {
    RecordCoverage cov;
    auto exp = mergeSpans(expected);
    auto covered = intersectSpans(exp, mergeSpans(recorded));
    size_t c = 0;
    for (const auto &e : exp) {
        cov.expected_sec += e.second - e.first;
        int64_t cursor = e.first;
        auto gap = [&](int64_t len) {
            if (len > gap_min_sec) {
                ++cov.gaps;
                cov.longest_gap_sec = std::max(cov.longest_gap_sec, len);
            }
        };
        for (; c < covered.size() && covered[c].first < e.second; ++c) {
            cov.recorded_sec += covered[c].second - covered[c].first;
            gap(covered[c].first - cursor);
            cursor = covered[c].second;
        }
        gap(e.second - cursor);
    }
    return cov;
}

double rssGrowthMbPerHour(const vector<pair<int64_t, int64_t> > &samples, int64_t min_span_sec) {
    if (samples.size() < 3 || samples.back().first - samples.front().first < min_span_sec) {
        return NAN;
    }
    // Hours from the first sample and MiB, to keep the sums small.
    const double t0 = (double)samples.front().first;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double n = (double)samples.size();
    for (const auto &s : samples) {
        double x = ((double)s.first - t0) / 3600.0;
        double y = (double)s.second / (1024.0 * 1024.0);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    double den = n * sxx - sx * sx;
    return den > 0 ? (n * sxy - sx * sy) / den : NAN;
}

double percentileOf(vector<uint32_t> values, double p) {
    if (values.empty()) {
        return NAN;
    }
    std::sort(values.begin(), values.end());
    size_t rank = (size_t)std::ceil(p / 100.0 * values.size());
    rank = std::max<size_t>(1, std::min(rank, values.size()));
    return (double)values[rank - 1];
}

static int overallRank(const string &level) {
    return level == "critical" ? 3 : (level == "warning" ? 2 : (level == "ok" ? 1 : 0));
}

Json::Value healthHistoryPoints(const vector<HealthMetric> &rows, int bucket_sec) {
    struct Bucket {
        int64_t ts = 0;
        int n = 0;
        string overall = "unknown";
        double score = 0;
        int score_min = 100;
        double cameras_total = 0, cameras_online = 0, rec_expected = 0, rec_active = 0, rec_stalled = 0, viewers = 0;
        double poller_max = 0;
        int alerts_max = 0;
        int64_t rss_max = 0;
    };
    vector<Bucket> buckets;
    for (const auto &r : rows) {
        int64_t ts = bucket_sec > 0 ? (r.timestamp / bucket_sec) * bucket_sec : r.timestamp;
        if (buckets.empty() || buckets.back().ts != ts || bucket_sec <= 0) {
            buckets.push_back(Bucket());
            buckets.back().ts = ts;
        }
        auto &b = buckets.back();
        ++b.n;
        if (overallRank(r.overall) > overallRank(b.overall)) {
            b.overall = r.overall;
        }
        b.score += r.score;
        b.score_min = std::min(b.score_min, r.score);
        b.cameras_total += r.cameras_total;
        b.cameras_online += r.cameras_online;
        b.rec_expected += r.rec_expected;
        b.rec_active += r.rec_active;
        b.rec_stalled += r.rec_stalled;
        b.viewers += r.viewers_total;
        b.poller_max = std::max(b.poller_max, (double)r.poller_delay_max_ms);
        b.alerts_max = std::max(b.alerts_max, r.alerts_active);
        b.rss_max = std::max(b.rss_max, r.proc_rss);
    }
    Json::Value out(Json::arrayValue);
    for (const auto &b : buckets) {
        auto avg = [&](double sum) { return std::round(sum / b.n * 100.0) / 100.0; };
        Json::Value p;
        p["timestamp"] = (Json::Int64)b.ts;
        p["samples"] = b.n;
        p["overall"] = b.overall;
        p["score"] = avg(b.score);
        p["score_min"] = b.score_min;
        p["cameras_total"] = avg(b.cameras_total);
        p["cameras_online"] = avg(b.cameras_online);
        p["rec_expected"] = avg(b.rec_expected);
        p["rec_active"] = avg(b.rec_active);
        p["rec_stalled"] = avg(b.rec_stalled);
        p["viewers_total"] = avg(b.viewers);
        p["poller_delay_max_ms"] = std::round(b.poller_max * 100.0) / 100.0;
        p["alerts_active"] = b.alerts_max;
        p["proc_rss"] = (Json::Int64)b.rss_max;
        out.append(p);
    }
    return out;
}

} // namespace managerkit

namespace managerkit {

void HealthMonitor::recordHistory(const Json::Value &health, const Json::Value &summary, int64_t now) {
    using namespace HealthMonitorConfig;
    GET_CONFIG(int, interval_sec, kHistoryIntervalSec);
    if (interval_sec <= 0 || !_db_queue) {
        return;
    }
    if (now - _history_cleanup_at >= 3600) {
        _history_cleanup_at = now;
        GET_CONFIG(int, retention_days, kHistoryRetentionDays);
        const int64_t cutoff = now - (int64_t)std::max(1, retention_days) * 86400;
        _db_queue->post([cutoff]() {
            try {
                HealthMetricsImp().deleteOlderThan(cutoff);
                ProcessStartImp().deleteOlderThan(cutoff);
            } catch (const std::exception &ex) {
                WarnL << "Health monitor: purging history failed: " << ex.what();
            }
        });
    }
    // A previous write still waits on a locked database: skip this sample rather than pile up.
    if (now - _history_at < interval_sec || _db_queue->pending() > 0) {
        return;
    }
    _history_at = now;
    HealthMetric m;
    m.timestamp = now;
    m.overall = health["overall"].asString();
    m.score = health["score"].asInt();
    m.cameras_total = summary["ingest"]["cameras"].asInt();
    m.cameras_online = summary["ingest"]["online"].asInt();
    m.rec_expected = summary["recording"]["expected"].asInt();
    m.rec_active = summary["recording"]["active"].asInt();
    m.rec_stalled = summary["recording"]["stalled"].asInt();
    m.viewers_total = summary["delivery"]["viewers"]["total"].asInt();
    const auto &delay = summary["process"]["pollers"]["delay_max_ms"];
    m.poller_delay_max_ms = delay.isNumeric() ? delay.asFloat() : 0;
    m.alerts_active = health["alerts"]["active"].asInt();
    m.proc_rss = (int64_t)summary["host"]["ram"]["proc_rss"].asUInt64();
    Json::FastWriter writer;
    m.components_json = writer.write(health["components"]);
    _db_queue->post([m]() {
        try {
            HealthMetricsImp().insertMetric(m);
        } catch (const std::exception &ex) {
            WarnL << "Health monitor: writing history failed: " << ex.what();
        }
    });
}

Json::Value HealthMonitor::history(int64_t from, int64_t to, int bucket_sec, const string &fields) {
    const int64_t span = std::max<int64_t>(1, to - from);
    // At most ~2000 points per series: a wider range is bucketed (whole minutes).
    int64_t min_bucket = (span + 1999) / 2000;
    if (min_bucket > 60) {
        min_bucket = (min_bucket + 59) / 60 * 60;
    } else {
        min_bucket = 0;
    }
    const int bucket = (int)std::max<int64_t>(bucket_sec > 0 ? bucket_sec : 0, min_bucket);

    set<string> wanted;
    for (auto &f : split(fields, ",")) {
        trim(f);
        if (!f.empty()) {
            wanted.insert(f);
        }
    }
    GET_CONFIG(string, media_server_id, General::kMediaServerId);
    Json::Value data;
    data["schemaVersion"] = 1;
    data["mediaServerId"] = media_server_id;
    data["from"] = (Json::Int64)from;
    data["to"] = (Json::Int64)to;
    data["bucket_sec"] = bucket;
    Json::Value points = healthHistoryPoints(HealthMetricsImp().findByTimeRange(from, to), bucket);
    if (!wanted.empty()) {
        for (auto &p : points) {
            for (const auto &name : p.getMemberNames()) {
                if (name != "timestamp" && name != "samples" && !wanted.count(name)) {
                    p.removeMember(name);
                }
            }
        }
    }
    data["points"] = points;
    if (wanted.empty() || wanted.count("system")) {
        data["system"] = GlobalMonitor::Instance().getSystemStatisticHistory(from, to, 0, bucket);
    }
    return data;
}

void HealthMonitor::startCoverage(int64_t now) {
    GET_CONFIG(int, interval_sec, HealthMonitorConfig::kCoverageIntervalSec);
    if (_coverage_started_at > 0 && now - _coverage_started_at < std::max(60, interval_sec)) {
        return;
    }
    if (_coverage_running.exchange(true)) {
        return;
    }
    _coverage_started_at = now;
    vector<CoverageCamera> cameras;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
        auto impl = std::dynamic_pointer_cast<GenericRtspCameraImp>(src->getListener().lock());
        if (!impl || !impl->isEnabled()) {
            return;
        }
        auto stats = impl->getCameraStatisticImp();
        if (!stats) {
            return;
        }
        auto params = stats->getParams();
        CoverageCamera c;
        c.camera_id = src->getDeviceTuple().device_id;
        c.enable_record = params.option.enableRecord;
        c.schedules = params.option.recordSchedules;
        cameras.push_back(c);
    });
    GET_CONFIG(int, max_second, Protocol::kMP4MaxSecond);
    const int stale_sec = recordStaleSec();
    const int gap_min_sec = 2 * std::max(1, max_second);
    BlockingExecutors::api()->async([cameras, now, stale_sec, gap_min_sec]() {
        try {
            computeCoverage(cameras, now, stale_sec, gap_min_sec);
        } catch (const std::exception &ex) {
            WarnL << "Health monitor: recording coverage failed: " << ex.what();
        }
        HealthMonitor::Instance()._coverage_running = false;
    });
}

void HealthMonitor::computeCoverage(const vector<CoverageCamera> &cameras, int64_t now, int stale_sec, int gap_min_sec) {
    GET_CONFIG(string, self_id, General::kMediaServerId);
    // The segment being written is not in the timeline yet.
    const int64_t to = now - stale_sec;
    const int64_t from = now - 86400;
    // Files of one recording follow each other within a second or two: such holes are not gaps.
    const int64_t join_sec = 5;
    map<string, CoverageResult> results;
    for (const auto &c : cameras) {
        if (!HealthMonitor::Instance()._running.load()) {
            return;
        }
        CoverageResult res;
        res.at = now;
        try {
            auto expected = scheduledRecordSpans(c.enable_record, recordModesByHour(c.schedules), from, to);
            if (!expected.empty()) {
                // Only the time this node owned the camera (failover hands it to another node).
                auto assignments = VmsResourceAssignmentImp().findAssignmentsOverlappingRange(c.camera_id, from, to);
                if (!assignments.empty()) {
                    vector<TimeSpan> owned;
                    for (const auto &a : assignments) {
                        if (a.owner_peer_id == self_id) {
                            owned.emplace_back(std::max(from, a.assigned_at), a.released_at == 0 ? to : std::min(to, a.released_at));
                        }
                    }
                    expected = intersectSpans(expected, mergeSpans(owned));
                }
            }
            vector<TimeSpan> recorded;
            if (!expected.empty()) {
                // One more day before the window tells a camera that recorded before from one that
                // started recording inside it (new camera, recording just enabled).
                const int64_t look_from = from - 86400;
                mediakit::MediaTuple tuple = { DEFAULT_VHOST, c.camera_id, "", "" };
                TimeQuery query(tuple, "", (uint64_t)look_from, (uint64_t)to);
                query.getRecordedTimePeriod((uint64_t)look_from, (uint64_t)to, [&](vector<TimeRange> &ranges) {
                    for (const auto &r : ranges) {
                        int64_t start = (int64_t)r.startTime, end = (int64_t)(r.startTime + r.duration);
                        if (!recorded.empty() && start - recorded.back().second <= join_sec) {
                            recorded.back().second = std::max(recorded.back().second, end);
                        } else {
                            recorded.emplace_back(start, end);
                        }
                    }
                });
                if (recorded.empty()) {
                    // Nothing for two days: not judged here, recording.stalled reports the camera.
                    expected.clear();
                } else if (recorded.front().first >= from) {
                    expected = intersectSpans(expected, { TimeSpan(recorded.front().first, to) });
                }
            }
            res.coverage = computeRecordCoverage(expected, recorded, gap_min_sec);
        } catch (const std::exception &ex) {
            WarnL << "Health monitor: coverage of " << c.camera_id << " failed: " << ex.what();
            continue;
        }
        results[c.camera_id] = res;
    }
    auto &self = HealthMonitor::Instance();
    lock_guard<mutex> lck(self._coverage_mtx);
    self._coverage.swap(results);
}

void HealthMonitor::syncBaseline(const string &camera_id, int type, const StreamQuality::Ptr &quality,
                                 const StreamQualitySnapshot &q) {
    if (!_db_queue) {
        return;
    }
    const string key = camera_id + "/" + to_string(type);
    if (q.baseline_at > 0 && q.fps_baseline > 0) {
        {
            lock_guard<mutex> lck(_baseline_mtx);
            _baseline_stored.erase(key);
        }
        auto it = _baseline_saved.find(key);
        if (it != _baseline_saved.end() && it->second == q.baseline_at) {
            return;
        }
        _baseline_saved[key] = q.baseline_at;
        StreamBaselineRow row;
        row.camera_id = camera_id;
        row.stream_type = type;
        row.format = q.format;
        row.fps = (float)q.fps_baseline;
        row.bitrate_kbps = (float)std::max(0.0, q.bitrate_baseline);
        row.learnt_at = q.baseline_at;
        _db_queue->post([row]() {
            try {
                StreamBaselineImp().upsert(row);
            } catch (const std::exception &ex) {
                WarnL << "Health monitor: saving a stream baseline failed: " << ex.what();
            }
        });
        return;
    }
    StreamBaselineRow stored;
    bool have = false;
    {
        lock_guard<mutex> lck(_baseline_mtx);
        if (!_baseline_loaded) {
            return;
        }
        auto it = _baseline_stored.find(key);
        if (it != _baseline_stored.end()) {
            stored = it->second;
            have = true;
            _baseline_stored.erase(it);
        }
    }
    if (have && quality && quality->restoreBaseline(stored.format, stored.fps, stored.bitrate_kbps, stored.learnt_at)) {
        _baseline_saved[key] = stored.learnt_at;
        return;
    }
    // Learning again (new codec/resolution, settings changed, long drift), or a stored baseline for
    // a format the stream no longer sends: the saved one no longer applies.
    const bool was_saved = _baseline_saved.erase(key) > 0;
    if (have || was_saved) {
        const string id = camera_id;
        _db_queue->post([id, type]() {
            try {
                StreamBaselineImp().removeStream(id, type);
            } catch (const std::exception &ex) {
                WarnL << "Health monitor: removing a stream baseline failed: " << ex.what();
            }
        });
    }
}

} // namespace managerkit
