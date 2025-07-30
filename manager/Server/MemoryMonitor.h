#ifndef SERVER_MEMORYMONITOR_H
#define SERVER_MEMORYMONITOR_H

#include "ResourceMonitor.h"

namespace managerkit {

struct MemoryInfo {
    uint64_t totalMemory = 0;
    uint64_t usageMemory = 0;
    uint64_t procUsageMemory = 0;
    double usagePct = 0;
    double procUsagePct = 0;
};

class MemoryCollector :  public MetricCollector<MemoryInfo> {
public:
    using Ptr = std::shared_ptr<MemoryCollector>;
    MemoryCollector(toolkit::EventPoller::Ptr poller) : MetricCollector<MemoryInfo>(poller) {}

private:
    void collect() override;

private:
    MemoryInfo _info; 
};

class MemoryMonitor : public ResourceMonitor {
public:
    using Ptr = std::shared_ptr<MemoryMonitor>;
    MemoryMonitor(toolkit::EventPoller::Ptr poller) : ResourceMonitor(ResourceType::MEMORY, poller) {
        start();
    }

    MemoryInfo getCurrentUsage();

private:
    void start() override;

private:
    MemoryCollector::Ptr _collector;
    MemoryInfo _info;
};

} // namespace managerkit

#endif // SERVER_MEMORYMONITOR_H