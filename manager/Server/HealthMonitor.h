#ifndef S3MANAGERKIT_HEALTHMONITOR_H
#define S3MANAGERKIT_HEALTHMONITOR_H

#include <atomic>
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

namespace managerkit {

// ──────────────────────────────────────────────────────────────────
// Configuration of the health monitor ("monitor." section)
// ──────────────────────────────────────────────────────────────────
namespace HealthMonitorConfig {
extern const std::string kEnable;          // 1: evaluate and serve /media/monitor/* (default 1)
extern const std::string kIntervalSec;     // evaluation period, seconds (default 10)
extern const std::string kSustainSamples;  // consecutive samples before a level change (default 3)
extern const std::string kGpuFallbackWindowSec; // a GPU->CPU fallback keeps "process" warning this long (default 300)

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

/**
 * Keys /media/monitor/config/set may change: the "monitor." keys above plus the existing
 * GlobalMonitor thresholds (CPU/RAM/disk capacity, storage IO). Anything else (api.secret,
 * reader limits that come from the license) is rejected.
 */
const std::vector<std::string> &editableKeys();
// warn/crit key pairs checked by validateChanges (warn < crit).
const std::vector<std::pair<std::string, std::string> > &thresholdPairs();
/**
 * Checks `changes` (key -> new value) merged over `current` (current value of a key).
 * Returns an empty string when valid, otherwise the reason (unknown key, not a number,
 * out of range, warn >= crit).
 */
std::string validateChanges(const std::map<std::string, std::string> &changes,
                            const std::function<std::string(const std::string &)> &current);
} // namespace HealthMonitorConfig

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
};

} // namespace managerkit

#endif // S3MANAGERKIT_HEALTHMONITOR_H
