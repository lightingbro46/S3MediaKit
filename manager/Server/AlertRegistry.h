#ifndef S3MANAGERKIT_ALERTREGISTRY_H
#define S3MANAGERKIT_ALERTREGISTRY_H

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include "json/json.h"

namespace managerkit {

/**
 * Open alerts and the last events, for /media/monitor/alerts: a monitor app opened later still
 * sees what is wrong now. Fed by the same broadcasts as the `system-event` webhook (with the same
 * eventCode), whether or not the webhook is enabled, plus the health monitor's own alerts
 * (RecordingStalled). Thread-safe.
 */
class AlertRegistry {
public:
    enum Level { RECOVERED = 0, WARNING = 1, CRITICAL = 2 };

    struct Alert {
        uint64_t seq = 0;
        std::string event_code;
        std::string family;      // groups a raise with its recovery, e.g. "Cpu", "DiskIo"
        std::string entity;      // mount point, path, pool, camera; empty for host-wide alerts
        Level level = WARNING;
        int64_t since = 0;       // first raise of the open alert (unix seconds)
        int64_t at = 0;          // time of this event
        double value = 0;
        double threshold = 0;
        Json::Value extra;       // event specific fields (mountPoint, poolId, cameraId, ...)

        Json::Value toJson() const;
    };

    static AlertRegistry &Instance();

    // Starts listening to the alert broadcasts (once).
    void start();

    // A raise (WARNING/CRITICAL) opens or updates the alert family+entity; RECOVERED closes it.
    void onEvent(const std::string &family, const std::string &entity, const std::string &event_code, Level level,
                 double value, double threshold, const Json::Value &extra = Json::Value(Json::objectValue),
                 int64_t now = 0);

    bool isActive(const std::string &family, const std::string &entity) const;

    // {"seq": last seq, "active": [...], "recent": [events with seq > since_seq, oldest first]}.
    Json::Value toJson(uint64_t since_seq, size_t max_recent = 500) const;

    size_t activeCount() const;
    // Open alerts at `level` or above.
    size_t activeCount(Level min_level) const;

    // Usage thresholds (warn, crit) of the mounts whose fill level retention manages (record
    // directory, tier file pools), set by the health monitor each cycle. A DiskWarning/DiskCritical
    // under them is retention at work: it is dropped, or lowered to a warning under crit.
    void setManagedDiskThresholds(const std::map<std::string, std::pair<double, double> > &thresholds);
    // false: drop this disk usage raise; otherwise `level` and `threshold` may be adjusted.
    bool filterDiskUsage(const std::string &mount_point, double usage, Level &level, double &threshold) const;

    // Tests.
    void clear();
    static const size_t kRecentCapacity = 500;

private:
    AlertRegistry() = default;

private:
    mutable std::mutex _mtx;
    uint64_t _seq = 0;
    bool _started = false;
    std::map<std::string, Alert> _active; // family|entity -> latest raise
    std::deque<Alert> _recent;
    std::map<std::string, std::pair<double, double> > _managed_disk; // mount -> (warn, crit)
};

} // namespace managerkit

#endif // S3MANAGERKIT_ALERTREGISTRY_H
