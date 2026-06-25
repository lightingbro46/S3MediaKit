#ifndef SERVER_HDDMONITOR_H
#define SERVER_HDDMONITOR_H

#include <string>
#include <memory>
#include <unordered_map>
#include "json/json.h"
#include "ResourceMonitor.h"
#include "Util/TimeTicker.h"

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

    Json::Value toJson() const {
        Json::Value dv;
        dv["name"]      = device;
        dv["mount"]     = mount_point;
        dv["used"]      = (Json::UInt64)used_bytes;
        dv["total"]     = (Json::UInt64)total_bytes;
        dv["used_pct"]  = sanitize_for_json(usage_pct);
        return dv;
    }
};

class HddCollector : public MetricCollector<DiskUsage> {
public:
    using Ptr = std::shared_ptr<HddCollector>;
    HddCollector(const std::string &name, toolkit::EventPoller::Ptr poller) 
        : MetricCollector(poller) {
        _info.name = name;
    }

private:
    void collect() override;

private:
    DiskUsage _info;
};

class HddMonitor : public ResourceMonitor {
public:
    using Ptr = std::shared_ptr<HddMonitor>;
    HddMonitor(toolkit::EventPoller::Ptr poller) : ResourceMonitor(ResourceType::HDD, poller) {
        start();
    }

    std::vector<DiskPartition> getCurrentUsage();

private:
    void start() override;

private:
    std::unordered_map<std::string, HddCollector::Ptr> _map_collector;
    std::unordered_map<std::string, DiskPartition> _map_result;
};

} // namespace managerkit

#endif // SERVER_HDDMONITOR_H