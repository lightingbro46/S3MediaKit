#ifndef S3MANAGERKIT_HEALTHMONITOR_H
#define S3MANAGERKIT_HEALTHMONITOR_H

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "json/json.h"
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "Common/BlockingExecutors.h"
#include "HealthEvaluator.h"
#include "Camera/StreamQuality.h"

namespace managerkit {

// ──────────────────────────────────────────────────────────────────
// Configuration of the health monitor ("monitor." section)
// ──────────────────────────────────────────────────────────────────
namespace HealthMonitorConfig {
extern const std::string kEnable;          // 1: evaluate and serve /media/monitor/* (default 1)
extern const std::string kIntervalSec;     // evaluation period, seconds (default 10)
extern const std::string kSustainSamples;  // consecutive samples before a level change (default 3)
extern const std::string kGpuFallbackWindowSec; // a GPU->CPU fallback keeps "process" warning this long (default 300)
extern const std::string kRecordStaleSec;  // no segment closed for this long = not recording; 0 = mp4_max_second * 1.5 + 60
extern const std::string kPlayWindowSec;   // window of the play request/failure counters (default 300)
extern const std::string kPlayMinRequests; // fewer requests in the window: failure rate not evaluated (default 20)
extern const std::string kBaselineWarmupSec; // a stream must be live this long before its fps/bitrate is learnt (default 120)
extern const std::string kBaselineWindowSec; // fps/bitrate samples averaged into the baseline (default 300)
extern const std::string kSlowIntervalSec;   // database checks (pools, jobs, write latency) on the API pool (default 60)
extern const std::string kIntegrationWindowSec; // window of the webhook counters and of a Kafka drop warning (default 300)

/**
 * One warn/crit pair of a numeric metric: config keys "monitor.<name>Warn" / "monitor.<name>Crit".
 * CPU/RAM/disk capacity and the storage IO thresholds are not here: they stay the GlobalMonitor
 * ones (single source of truth).
 */
struct ThresholdDef {
    const char *name;      // config name, e.g. "load1PerCore"
    const char *metric;    // metric of the API, e.g. "cpu.load1_per_core"
    double warn;
    double crit;
    bool higher_is_worse;
};
const std::vector<ThresholdDef> &thresholdDefs();
std::string warnKey(const ThresholdDef &def);
std::string critKey(const ThresholdDef &def);
// Current warn/crit of `name` from config; defaults when the name is unknown.
HealthThreshold threshold(const std::string &name);
StreamBaselineConfig baselineConfig();

/**
 * Keys /media/monitor/config/set may change: the "monitor." keys above plus the existing
 * GlobalMonitor thresholds (CPU/RAM/disk capacity, storage IO). Anything else (api.secret,
 * reader limits that come from the license) is rejected.
 */
const std::vector<std::string> &editableKeys();
// warn/crit key pairs checked by validateChanges: warn < crit, or warn > crit when lower is worse.
struct ThresholdPair {
    std::string warn_key;
    std::string crit_key;
    bool higher_is_worse;
};
const std::vector<ThresholdPair> &thresholdPairs();
/**
 * Checks `changes` (key -> new value) merged over `current` (current value of a key).
 * Returns an empty string when valid, otherwise the reason (unknown key, not a number,
 * out of range, warn >= crit).
 */
std::string validateChanges(const std::map<std::string, std::string> &changes,
                            const std::function<std::string(const std::string &)> &current);
} // namespace HealthMonitorConfig

/**
 * Recording state of one camera (section 4.4 of the plan): `expected` comes from the record
 * scheduler, `last_write` is when a segment of the camera last closed on disk.
 */
enum class RecordingState {
    NOT_EXPECTED = 0, // schedule (or enableRecord) says no recording now
    STARTING = 1,     // expected for less than stale_sec: the first segment may not be closed yet
    ACTIVE = 2,       // a segment closed within stale_sec
    STALLED = 3,      // expected, past the grace time, and nothing written within stale_sec
};
const char *recordingStateString(RecordingState state);
RecordingState evaluateRecording(bool expected, int64_t expected_since, int64_t last_write, int64_t now, int stale_sec);
// Default stale time: a segment closes every mp4_max_second, plus margin for the close itself.
int defaultRecordStaleSec(int mp4_max_second);

// Level of a tier pool from TierStorageManager's health check, which already applies its own
// failure/recovery thresholds: OFFLINE/CRITICAL = critical, HIGH/WARNING = warning, stale = unknown.
HealthLevel poolHealthLevel(const std::string &health_status, bool stale);
/**
 * Failure rate of calls to an outside system (webhooks) in a window. With fewer than
 * `min_requests` calls a rate says little: any failure is a warning, never critical.
 */
HealthLevel failureRateLevel(uint64_t requests, uint64_t failures, uint64_t min_requests,
                             const HealthThreshold &th, double &failure_pct);

/**
 * Per camera evaluation (section 5.4): offline, not recording while it should, or no video on a
 * live stream = critical; fps/bitrate below the learnt baseline, RTP loss, reconnects, low uptime,
 * controller (ONVIF/ISAPI/PSIA) not connected = warning.
 */
struct CameraStreamInput {
    bool present = false;          // configured and not disabled
    bool live = false;
    double last_frame_age_ms = -1; // -1 = unknown
    double fps_ratio = -1;         // -1 = no baseline yet
    double bitrate_ratio = -1;
    double loss_pct = -1;
    int disconnects_1h = 0;
    double uptime_pct_24h = -1;
};
struct CameraInput {
    bool recording_stalled = false;
    bool controller_expected = false; // not a generic RTSP camera
    bool controller_connected = true;
    CameraStreamInput streams[2];     // primary, secondary
};
struct CameraThresholds {
    HealthThreshold fps_ratio, bitrate_ratio, loss_pct, last_frame_age_ms, disconnects_1h, uptime_pct_24h;
    static CameraThresholds fromConfig();
};
// Online = at least one stream live (like the device statistics API).
bool cameraOnline(const CameraInput &in);
// True when a live stream sent no video for at least the critical frame age.
bool cameraNoVideo(const CameraInput &in, const CameraThresholds &th);
HealthLevel evaluateCamera(const CameraInput &in, const CameraThresholds &th, std::vector<std::string> &reasons);

/**
 * The result of one evaluation cycle. Immutable once published; the API serializes it without
 * touching any monitor or camera (N1).
 */
struct HealthSnapshot {
    using Ptr = std::shared_ptr<const HealthSnapshot>;
    int64_t generated_at = 0;   // unix seconds
    uint64_t rev = 0;           // bumped when levels or reasons change
    HealthEvaluation eval;
    Json::Value health;         // data of /media/monitor/health (without per-request fields)
    Json::Value summary;        // data of /media/monitor/summary
    // Cameras of /media/monitor/cameras, most severe first; each has "health".
    std::vector<Json::Value> cameras;
};

/**
 * Evaluates the server health every `monitor.intervalSec` (docs/api-health-monitor.md).
 * Runs on its own poller; reads only thread-safe getters (GlobalMonitor, BlockingExecutors,
 * transcode counters) and publishes a HealthSnapshot under a mutex.
 */
class HealthMonitor {
public:
    static HealthMonitor &Instance();

    void start();
    void stop();
    bool running() const { return _running.load(); }

    // Latest snapshot; nullptr before the first cycle or when disabled.
    HealthSnapshot::Ptr snapshot() const;

    /**
     * BlockingExecutors::stats() resets its window on every call, so the monitor is its only
     * periodic caller and keeps the last result here; `/index/api/getBlockingExecutors` reads it.
     * Returns false (and leaves `out` empty) when the monitor is not running.
     */
    bool blockingStats(std::vector<BlockingExecutors::PoolStats> &out) const;

    // Run one cycle now (tests, and the first sample at start). Must run on the monitor poller
    // or before start().
    void collectNow();

    // Process start, unix seconds.
    int64_t startedAt() const { return _started_at; }

    // A segment of `camera_id` closed on disk (kBroadcastRecordMP4). Any thread.
    void onRecordWritten(const std::string &camera_id, uint64_t bytes);
    // Result of a play authorization (kBroadcastMediaPlayed): empty = allowed. Any thread.
    void onPlayResult(const std::string &err);
    // Final result of a webhook call to the VMS/ESC (after its retries). Any thread.
    void onWebhookResult(bool ok);

private:
    HealthMonitor();

    struct PollerDelay {
        bool valid = false;
        int64_t sampled_ms = 0;   // steady clock of the last answer
        int64_t requested_ms = 0; // steady clock of the last request
        int delay_max_ms = 0;
        double delay_avg_ms = 0;
        int load_max = 0;
        size_t count = 0;
    };
    void requestPollerDelays();
    void onPollerDelay(bool work_pool, const std::vector<int> &delays, const std::vector<int> &loads);
    // Delay to evaluate: the measured one, or the waiting time when the answer is overdue
    // (a poller stuck in a task never answers).
    static double effectiveDelayMs(const PollerDelay &d, int64_t now_ms);

    void collectHost(HealthEvaluator &ev, Json::Value &summary, int64_t now);
    void collectProcess(HealthEvaluator &ev, Json::Value &summary, int64_t now);
    void collectStorage(HealthEvaluator &ev, Json::Value &summary, int64_t now);
    void collectRecording(HealthEvaluator &ev, Json::Value &summary, int64_t now);
    void collectIngest(HealthEvaluator &ev, Json::Value &summary, std::vector<Json::Value> &cameras, int64_t now);
    void collectDelivery(HealthEvaluator &ev, Json::Value &summary, int64_t now);
    void collectCluster(HealthEvaluator &ev, Json::Value &summary, int64_t now);
    // Database part (SQLite) on BlockingExecutors::api(); the cycle evaluates its last result.
    void startSlowCollect(int64_t now);
    void slowCollect();
    void evaluateSlow(HealthEvaluator &ev, Json::Value &summary, int64_t now);

private:
    std::atomic<bool> _running{false};
    int64_t _started_at = 0;
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;

    mutable std::mutex _snap_mtx;
    HealthSnapshot::Ptr _snapshot;
    std::vector<BlockingExecutors::PoolStats> _blocking_stats;

    mutable std::mutex _delay_mtx;
    PollerDelay _pollers;
    PollerDelay _work_pollers;

    // State below is touched only on _poller.
    HealthHysteresis _hysteresis;
    uint64_t _cycle = 0;
    uint64_t _rev = 0;
    std::string _last_signature;
    struct NetCounters {
        uint64_t errors = 0;
        uint64_t drops = 0;
        int64_t at_ms = 0;
    };
    std::map<std::string, NetCounters> _net_counters;
    uint64_t _gpu_fallbacks = 0;
    int64_t _gpu_fallback_at = 0; // unix seconds of the last increase
    uint64_t _write_errors = 0;   // MP4Recorder::writeErrorCount() at the previous cycle
    int _write_error_cycles = 0;  // consecutive cycles with new write errors
    std::map<std::string, RecordingState> _recording_states; // camera -> state of the previous cycle
    struct PlayBucket {
        uint64_t requests = 0;
        uint64_t failures = 0;
    };
    std::deque<PlayBucket> _play_buckets;   // one per cycle, newest last
    std::deque<PlayBucket> _webhook_buckets;
    int64_t _slow_started_at = 0;  // unix seconds of the last slowCollect() dispatch
    uint64_t _kafka_dropped = 0;   // EventBus drops at the previous cycle
    int64_t _kafka_dropped_at = 0; // unix seconds of the last increase
    // Sync peer -> wall clock ms it was first seen, the reference before its first complete pull.
    std::map<std::string, uint64_t> _sync_peer_seen_ms;

    // Written from record-close threads and session pollers.
    mutable std::mutex _record_mtx;
    struct RecordWrite {
        int64_t last_write = 0;
        uint64_t bytes = 0;
    };
    std::map<std::string, RecordWrite> _record_writes;
    std::mutex _play_mtx;
    PlayBucket _play_current;
    std::map<std::string, uint64_t> _play_failure_reasons; // since start
    std::mutex _webhook_mtx;
    PlayBucket _webhook_current;

    // Result of the last slowCollect(), written on the API pool.
    struct PoolView {
        std::string id, name, tier, type;
        bool enabled = true;
        bool health_check = true;   // health_check_enabled of the pool
        std::string health_status;  // UNKNOWN | OK | WARNING | HIGH | CRITICAL | OFFLINE
        bool health_known = false;
        bool stale = true;
        float usage_pct = 0;
        std::string message;
        bool throughput_known = false;
        bool throughput_low = false;
        float write_mbps = 0;
        int64_t backlog_bytes = 0;
        int64_t backlog_eta_sec = -1;
    };
    struct SlowResult {
        bool valid = false;
        int64_t at = 0;
        double db_write_ms = -1;   // BEGIN IMMEDIATE on the MediaServer database, -1 = failed
        std::string db_error;
        bool tier = false;
        std::vector<PoolView> pools;
        int tiering_failed_24h = 0, tiering_pending = 0, tiering_running = 0;
        int restore_failed_24h = 0, restore_failed_1h = 0, restore_pending = 0;
    };
    mutable std::mutex _slow_mtx;
    SlowResult _slow;
    std::atomic<bool> _slow_running{false};
};

} // namespace managerkit

#endif // S3MANAGERKIT_HEALTHMONITOR_H
