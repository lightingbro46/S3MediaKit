#include "GlobalMonitor.h"
#include "Util/util.h"
#include "Common/macros.h"
#include <iomanip>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(GlobalMonitor)

GlobalMonitor::GlobalMonitor() {
    _poller = EventPollerPool::Instance().getPoller();
    _info = get_os_info();
}

GlobalMonitor::~GlobalMonitor() {
    _timer.reset();
}

static std::string format_double_2f(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value;
    return oss.str();
}

static std::string format_bytes_human_readable(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double size = static_cast<double>(bytes);
    int unit_index = 0;

    while (size >= 1024.0 && unit_index < 5) {
        size /= 1024.0;
        ++unit_index;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    return oss.str();
}

void GlobalMonitor::start() {
    _cpu_monitor = std::make_shared<CpuMonitor>(_poller);
    _cpu_monitor->start();
    DebugL << "Start monitoring CPU usage";

    _mem_monitor = std::make_shared<MemoryMonitor>(_poller);
    _mem_monitor->start();
    DebugL << "Start monitoring Memory usage";

    _net_monitor = std::make_shared<NetworkMonitor>(_poller);
    _net_monitor->start();
    DebugL << "Start monitoring Network usage";

    _hdd_monitor = std::make_shared<HddMonitor>(_poller);
    _hdd_monitor->start();
    DebugL << "Start monitoring Hdd usage";

    weak_ptr<GlobalMonitor> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(
        600.0f,
        [=]() { 
            if (auto strong_self = weak_self.lock()) {
                auto cpu_usage = strong_self->_cpu_monitor->getCurrentUsage();
                auto mem_usage = strong_self->_mem_monitor->getCurrentUsage();
                auto net_usage = strong_self->_net_monitor->getCurrentUsage();
                auto hdd_usage = strong_self->_hdd_monitor->getCurrentUsage();

                DebugL << "OS CPU usage: " << format_double_2f(cpu_usage.usagePct) << "%";
                DebugL << "Process CPU usage: " << format_double_2f(cpu_usage.procUsagePct) << "%";
                DebugL << "OS Memory usage: " << format_double_2f(mem_usage.usagePct) << "%";
                DebugL << "Process Memory usage: " << format_double_2f(mem_usage.procUsagePct) << "%";
                DebugL << "Network usage:";
                for (const auto &it : net_usage) {
                    DebugL << "     " << it.name << " - in " << format_double_2f(it.rx_mbps) << " Mbps, out " << format_double_2f(it.tx_mbps) << " Mbps";
                }
                DebugL << "HDD usage:";
                for (const auto &it : hdd_usage) {
                    DebugL << "     " << it.mount_point << " - total " << format_bytes_human_readable(it.total_bytes) 
                                                        << ", used " << format_bytes_human_readable(it.used_bytes) 
                                                        << ", usage " << format_double_2f(it.usage_pct) << "%";
                }
            }
            return true;
        },
        _poller);
}

OSInfo GlobalMonitor::getOsInfo() {
    return _info;
}

CpuInfo GlobalMonitor::getCpuUsage() {
    return _cpu_monitor->getCurrentUsage();
}

MemoryInfo GlobalMonitor::getMemUsage() {
    return _mem_monitor->getCurrentUsage();
}

vector<NetInterfaceInfo> GlobalMonitor::getNetUsage() {
    return _net_monitor->getCurrentUsage();
}

vector<DiskPartition> GlobalMonitor::getHddUsage() {
    return _hdd_monitor->getCurrentUsage();
}

} // namespace managerkit 