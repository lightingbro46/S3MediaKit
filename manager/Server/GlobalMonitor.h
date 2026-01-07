#ifndef S3MANAGERKIT_GLOBALMONITOR_H
#define S3MANAGERKIT_GLOBALMONITOR_H

#include "CpuMonitor.h"
#include "HddMonitor.h"
#include "MemoryMonitor.h"
#include "NetworkMonitor.h"
#include "ReaderMonitor.h"
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

    std::string getLocalIps();

    std::string getMacAddresses();

    void setThreshold(const ResourceType &type, double warning_threshold = -1, double critical_threshold = -1);

    std::pair<double, double> getThreshold(const ResourceType &type);

    void setStreamReaderCount(const std::string &camera_id, int reader_count, bool record_stream = false);

    bool isReaderCountLimit(const std::string &camera_id, bool record_stream = false);

    ReaderCountInfoMap getReaderUsage();

private:
    GlobalMonitor();

private:
    OSInfo _info;
    CpuMonitor::Ptr _cpu_monitor;
    MemoryMonitor::Ptr _mem_monitor;
    NetworkMonitor::Ptr _net_monitor;
    HddMonitor::Ptr _hdd_monitor;
    ReaderMonitor::Ptr _reader_monitor;

    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
};

} // namespace managerkit

#endif // S3MANAGERKIT_GLOBALMONITOR_H