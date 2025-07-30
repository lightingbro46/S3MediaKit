#ifndef SERVER_CPUMONITOR_H
#define SERVER_CPUMONITOR_H

#include "ResourceMonitor.h"

namespace managerkit {

struct CpuInfo {
    int cores = 0;
    double usagePct = 0.0;
    double procUsagePct = 0.0;
};

class CpuCollector : public MetricCollector<CpuInfo> {
public:
    using Ptr = std::shared_ptr<CpuCollector>;
    CpuCollector(toolkit::EventPoller::Ptr poller) : MetricCollector<CpuInfo>(poller) {}

private:
    void collect() override;

private:
    CpuInfo _info;
};

class CpuMonitor : public ResourceMonitor {
public:
    using Ptr = std::shared_ptr<CpuMonitor>;
    CpuMonitor(toolkit::EventPoller::Ptr poller) : ResourceMonitor(ResourceType::CPU, poller) {
        start();
    }

    CpuInfo getCurrentUsage();

private:
    void start() override;

private:
    CpuCollector::Ptr _collector;
    CpuInfo _info;
};

} // namespace managerkit 


#endif // SERVER_CPUMONITOR_H