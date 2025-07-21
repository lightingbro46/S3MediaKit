#ifndef S3MANAGERKIT_GLOBALMONITOR_H
#define S3MANAGERKIT_GLOBALMONITOR_H

#include "CpuMonitor.h"
#include "MemoryMonitor.h"
#include "NetworkMonitor.h"
#include "HddMonitor.h"
#include "osInfo.h"

namespace managerkit {

class GlobalMonitor : public std::enable_shared_from_this<GlobalMonitor> {
public:
    using Ptr = std::shared_ptr<GlobalMonitor>;

    static GlobalMonitor &Instance();
    ~GlobalMonitor();

    void start();

    OSInfo getOsInfo();

    CpuInfo getCpuUsage();

    MemoryInfo getMemUsage();

    std::vector<NetInterfaceInfo> getNetUsage();

    std::vector<DiskPartition> getHddUsage();

private:
    GlobalMonitor();

private:
    OSInfo _info;
    CpuMonitor::Ptr _cpu_monitor;
    MemoryMonitor::Ptr _mem_monitor;
    NetworkMonitor::Ptr _net_monitor;
    HddMonitor::Ptr _hdd_monitor;

    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
};

} // namespace managerkit

#endif // S3MANAGERKIT_GLOBALMONITOR_H