#ifndef SERVER_HDDMONITOR_H
#define SERVER_HDDMONITOR_H

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>
#include "json/json.h"
#include "ResourceMonitor.h"
#include "Util/TimeTicker.h"
#include "Util/util.h"

namespace managerkit {

struct DiskUsage {
    std::string name;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t used_bytes = 0;
    float usage_pct = 0.0f;
};

struct DiskPartition {
    std::string device;     // Ví dụ: "/dev/sda1" hoặc "C:\\"
    std::string mount_point;
    std::string filesystem_type;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t used_bytes = 0;
    float usage_pct = 0.0f;
    bool read_only = false;
    int64_t last_seen_at = 0;

    Json::Value toJson() const {
        Json::Value dv;
        dv["name"]      = device;
        dv["mount"]     = mount_point;
        dv["filesystem_type"] = filesystem_type;
        dv["read_only"] = read_only;
        dv["last_seen_at"] = static_cast<Json::Int64>(last_seen_at);
        dv["used"]      = (Json::UInt64)used_bytes;
        dv["total"]     = (Json::UInt64)total_bytes;
        dv["used_pct"]  = sanitize_for_json(usage_pct);
        return dv;
    }

    bool isNetworkFileSystem() const {
        static const std::set<std::string> network_fs = {
            "nfs", "nfs4", "cifs", "smb", "smb2", "smb3",
            "fuse.sshfs", "davfs", "glusterfs", "ceph", "lustre"
        };
        return network_fs.count(toolkit::strToLower(static_cast<std::string>(filesystem_type))) > 0;
    }
};

struct DiskSnapshot {
    std::vector<DiskPartition> partitions;
    int64_t refreshed_at = 0;
    uint64_t generation = 0;
    bool valid = false;
};

class HddMonitor : public ResourceMonitor,
                   public std::enable_shared_from_this<HddMonitor> {
public:
    using Ptr = std::shared_ptr<HddMonitor>;
    using PartitionProvider = std::function<bool(std::vector<DiskPartition> &)>;

    explicit HddMonitor(toolkit::EventPoller::Ptr poller,
                        float refresh_interval_sec = 10.0f,
                        const PartitionProvider &provider = PartitionProvider());
    ~HddMonitor() override;

    void start() override;

    bool refreshNow();

    DiskSnapshot getSnapshot();

    std::vector<DiskPartition> getCurrentUsage();

    static bool findMountPoint(const DiskSnapshot &snapshot,
                               const std::string &path,
                               DiskPartition &out_partition);

    // While a mount stays above a threshold, repeat its alert at most this often (<= 0: never).
    void setAlertRepeatSec(int sec) { _alert_repeat_sec = sec; }

private:
    void dispatchRefresh();
    void emitUsageAlerts(const DiskSnapshot &snapshot);

private:
    float _refresh_interval_sec = 10.0f;
    PartitionProvider _provider;
    toolkit::Timer::Ptr _refresh_timer;
    std::mutex _refresh_mtx;
    DiskSnapshot _snapshot;
    std::atomic<bool> _refresh_running{false};
    std::atomic<bool> _stopping{false};
    std::atomic<int> _alert_repeat_sec{300};
    struct UsageAlertState {
        int level = 0;          // 0 normal, 1 warning, 2 critical
        int64_t last_emit = 0;  // unix seconds
    };
    // Guarded by _refresh_mtx: refreshNow() is the only writer.
    std::unordered_map<std::string, UsageAlertState> _usage_alerts;
};

} // namespace managerkit

#endif // SERVER_HDDMONITOR_H