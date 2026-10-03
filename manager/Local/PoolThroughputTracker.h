#ifndef LOCAL_POOL_THROUGHPUT_TRACKER_H
#define LOCAL_POOL_THROUGHPUT_TRACKER_H

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "json/json.h"
#include "Server/ResourceMonitor.h"

namespace managerkit {

struct PoolThroughputConfig {
    int window_sec = 3600;        // sliding window for every rate
    float safety_factor = 1.2f;   // capacity must stay above demand * factor
    int min_busy_sec = 60;        // capacity is unknown until transfers ran this long in the window
    int sustain_samples = 3;      // evaluations at a new state before it is reported
    int alert_repeat_sec = 300;   // repeat while low (<= 0: never)
};

// Throughput of one pool over the window. Rates are MiB/s, like DiskIoMonitor.
struct PoolThroughputStats {
    std::string pool_id;
    // Tier moves and uploads into the pool, measured over the time a transfer was running.
    bool write_valid = false;
    float write_mbps = 0.0f;
    float write_busy_pct = 0.0f;
    uint64_t write_bytes = 0;
    // Tier moves out of the pool and restores downloaded from it.
    bool read_valid = false;
    float read_mbps = 0.0f;
    float read_busy_pct = 0.0f;
    uint64_t read_bytes = 0;
    // New recordings landing on the pool (HOT pools only).
    float record_ingest_mbps = 0.0f;
    // Average traffic over the window, idle time included: recordings + transfers in, transfers out.
    float avg_write_mbps = 0.0f;
    float avg_read_mbps = 0.0f;
    // Recording rate of the cameras whose policy moves data into the pool.
    bool demand_valid = false;
    float demand_mbps = 0.0f;
    int demand_cameras = 0;
    int64_t backlog_bytes = 0;    // queued and running transfers into the pool
    int64_t backlog_eta_sec = -1; // backlog / write_mbps, -1 when unknown
    bool low = false;             // confirmed (sustained) state

    Json::Value toJson() const;
};

// Recording rate landing on one pool (or the tier-off record directory) over the window.
struct PoolIngestRates {
    bool valid = false;                        // observed for min(window, 15 min)
    float mbps = 0.0f;                         // every camera together
    uint64_t bytes = 0;
    std::map<std::string, float> camera_mbps;  // cameras that recorded into it in the window
};

// A pool entered (low = true) or left the low-throughput state, or is still low after the repeat interval.
struct PoolThroughputAlert {
    PoolThroughputStats stats;
    float required_mbps = 0.0f;   // demand * safety_factor
};

/**
 * Compares the rate a pool accepts tier transfers with the rate data must flow into it.
 *
 * Capacity is bytes written into the pool divided by the time at least one transfer into it was
 * running, so parallel jobs count once and idle time does not lower it. Demand is the recording
 * rate of the cameras routed into the pool: in steady state every recorded byte crosses each tier
 * boundary once, and unlike the job queue it does not spike when a backlog is queued at once.
 *
 * Thread-safe; callers pass a steady-clock time in ms.
 */
class PoolThroughputTracker {
public:
    enum Direction { WRITE = 0, READ = 1 };

    void beginTransfer(const std::string &pool_id, Direction dir, int64_t now_ms);
    // bytes = 0 for a failed transfer: its time still counts, a pool that keeps failing is not fast.
    void endTransfer(const std::string &pool_id, Direction dir, uint64_t bytes, int64_t now_ms);
    void addIngest(const std::string &camera_id, const std::string &hot_pool_id, uint64_t bytes, int64_t now_ms);
    // Computed on the spot from the ingest meters (also prunes them, so it works without evaluate()).
    PoolIngestRates ingestRates(const std::string &pool_id, int window_sec, int64_t now_ms);
    // Target pool -> cameras whose policy moves recordings into it; replaces the previous routes.
    void setRoutes(const std::map<std::string, std::set<std::string>> &routes);

    // Recomputes every pool and returns the alerts to raise. backlog: target pool -> bytes.
    std::vector<PoolThroughputAlert> evaluate(const std::map<std::string, int64_t> &backlog,
                                              const PoolThroughputConfig &cfg,
                                              int64_t now_ms);

    // Result of the last evaluate().
    bool getStats(const std::string &pool_id, PoolThroughputStats &out) const;
    void removePool(const std::string &pool_id);

private:
    struct Bucket {
        int64_t start_ms = 0;
        uint64_t bytes = 0;
        int64_t busy_ms = 0;
    };
    struct Meter {
        std::deque<Bucket> buckets;
        int active = 0;
        int64_t busy_since_ms = 0;

        void creditBusy(int64_t now_ms);
        Bucket &bucketAt(int64_t at_ms);
        void prune(int64_t oldest_ms);
        void sum(int64_t oldest_ms, uint64_t &bytes, int64_t &busy_ms) const;
    };
    struct PoolState {
        Meter write;
        Meter read;
        Meter ingest;
        std::map<std::string, Meter> camera_ingest;
        AlertLevelTracker alert;
    };

    PoolState &pool(const std::string &pool_id);
    void pruneCameras(int64_t oldest_ms);

private:
    mutable std::mutex _mtx;
    int64_t _started_ms = -1;
    std::map<std::string, PoolState> _pools;
    std::map<std::string, Meter> _cameras;
    std::map<std::string, std::set<std::string>> _routes;
    std::map<std::string, PoolThroughputStats> _stats;
};

// Marks one transfer as running on its pools until finish() or destruction (counted as failed).
class PoolTransferScope {
public:
    PoolTransferScope(PoolThroughputTracker &tracker, const std::string &write_pool, const std::string &read_pool);
    ~PoolTransferScope();
    void finish(uint64_t bytes);

private:
    PoolTransferScope(const PoolTransferScope &) = delete;
    PoolTransferScope &operator=(const PoolTransferScope &) = delete;

    PoolThroughputTracker &_tracker;
    std::string _write_pool;
    std::string _read_pool;
    bool _done = false;
};

} // namespace managerkit

#endif // LOCAL_POOL_THROUGHPUT_TRACKER_H
