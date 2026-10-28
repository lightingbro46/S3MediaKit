#ifndef LOCAL_RETENTION_FORECAST_H
#define LOCAL_RETENTION_FORECAST_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "json/json.h"
#include "Server/ResourceMonitor.h"

namespace managerkit {

// Recording rate of one camera and how long its recordings must stay (0: no requirement).
struct RetentionCameraDemand {
    std::string camera_id;
    float mbps = 0.0f;
    int64_t required_sec = 0;
};

struct RetentionForecastInput {
    bool ingest_valid = false;   // recording rate observed long enough
    float ingest_mbps = 0.0f;    // every camera writing to the storage, MiB/s
    int64_t total_bytes = 0;
    int64_t used_bytes = 0;
    int usable_percent = 100;    // deletion / tier-move threshold
    // Target: the configured retention (upper bounds: retain_until_days, delete_after_days,
    // keepArchivedMaxFor). Short of it is level 1: recordings leave earlier than configured.
    std::vector<RetentionCameraDemand> cameras;
    // Commitment: keepArchivedMinFor set by hand. Short of it reports short_level.
    std::vector<RetentionCameraDemand> committed;
    int short_level = 2;
};

// How long the storage holds recordings at the current rate. Days are -1 when unknown.
struct RetentionForecast {
    bool valid = false;
    float ingest_mbps = 0.0f;
    int64_t usable_bytes = 0;
    int64_t used_bytes = 0;
    int64_t required_bytes = 0;      // sum of rate * configured retention (target)
    int cameras = 0;                 // cameras with a target
    int64_t committed_bytes = 0;     // sum of rate * keepArchivedMinFor (commitment)
    int committed_cameras = 0;       // cameras with a commitment
    double retention_days_estimate = -1;   // usable / daily ingest
    double estimated_remaining_days = -1;  // (usable - used) / daily ingest
    double required_days = -1;             // required / daily ingest
    double committed_days = -1;            // committed / daily ingest
    int observed = -1;               // this evaluation: -1 unknown, 0 fits, else the short level
    int level = 0;                   // confirmed (sustained) level

    Json::Value toJson() const;
};

RetentionForecast computeRetentionForecast(const RetentionForecastInput &in, float safety_factor);

// One pool a camera's recordings pass through, in tier order (tier storage on).
struct RetentionTierStay {
    std::string pool_id;
    int tier = 0;                // TierType: pools are evaluated HOT, then WARM, then COLD
    int64_t sec = 0;             // how long the recordings stay there; 0 = no requirement
    bool spill = false;          // what does not fit moves on to the next stay (MOVE_TO_NEXT_TIER)
    std::string overflow_action;
};

struct RetentionCameraPlan {
    std::string camera_id;
    float mbps = 0.0f;
    std::vector<RetentionTierStay> stays;
};

// What one pool must hold in steady state: its own stays plus what the previous tier pushed down.
struct RetentionPoolDemand {
    float inflow_mbps = 0.0f;                    // cameras whose recordings pass through the pool
    double required_bytes = 0;
    std::map<std::string, double> camera_bytes;  // cameras with a requirement on the pool
    int short_level = 0;                         // 0 fits, 1 the excess moves on, 2 recordings are lost
    std::string overflow_action;                 // of a camera losing recordings, else MOVE_TO_NEXT_TIER
};

/**
 * Bytes each pool must hold for every camera to keep its configured retention. A pool short of
 * usable / safety_factor pushes the excess, split by each camera's share, to that camera's next
 * stay when it moves on, or loses it. usable_bytes: pools with a known capacity only, the others
 * take everything.
 */
std::map<std::string, RetentionPoolDemand> planRetentionDemand(const std::vector<RetentionCameraPlan> &cameras,
                                                               const std::map<std::string, int64_t> &usable_bytes,
                                                               float safety_factor);

/**
 * Keeps the last forecast and the alert state per storage (record directory or HOT pool).
 * Thread-safe: evaluated on the storage pool, read by the API.
 */
class RetentionForecastBook {
public:
    // Stores the forecast; true when an alert (or recovery, level 0) must be emitted now.
    bool update(const std::string &key, RetentionForecast &forecast, int sustain, int64_t repeat_ms, int64_t now_ms);
    bool get(const std::string &key, RetentionForecast &out) const;
    void remove(const std::string &key);

private:
    struct Entry {
        RetentionForecast forecast;
        AlertLevelTracker alert;
    };
    mutable std::mutex _mtx;
    std::map<std::string, Entry> _entries;
};

} // namespace managerkit

#endif // LOCAL_RETENTION_FORECAST_H
