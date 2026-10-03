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
    int short_level = 1;         // level reported when the retention does not fit
    std::vector<RetentionCameraDemand> cameras;
};

// How long the storage holds recordings at the current rate. Days are -1 when unknown.
struct RetentionForecast {
    bool valid = false;
    float ingest_mbps = 0.0f;
    int64_t usable_bytes = 0;
    int64_t used_bytes = 0;
    int64_t required_bytes = 0;      // sum of rate * required time of the cameras with a requirement
    int cameras = 0;                 // cameras with a requirement
    double retention_days_estimate = -1;   // usable / daily ingest
    double estimated_remaining_days = -1;  // (usable - used) / daily ingest
    double required_days = -1;             // required / daily ingest
    int observed = -1;               // this evaluation: -1 unknown, 0 fits, else the short level
    int level = 0;                   // confirmed (sustained) level

    Json::Value toJson() const;
};

RetentionForecast computeRetentionForecast(const RetentionForecastInput &in, float safety_factor);

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
