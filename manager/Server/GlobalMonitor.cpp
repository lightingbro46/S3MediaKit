#include "GlobalMonitor.h"
#include "Common/macros.h"

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

void GlobalMonitor::start() {
    if (_timer) {
        WarnL << "Global monitor has been running. Ignore";
        return;
    }

    _cpu_monitor = std::make_shared<CpuMonitor>(_poller);
    DebugL << "Start monitoring CPU usage";

    _mem_monitor = std::make_shared<MemoryMonitor>(_poller);
    DebugL << "Start monitoring Memory usage";

    _net_monitor = std::make_shared<NetworkMonitor>(_poller);
    DebugL << "Start monitoring Network usage";

    _hdd_monitor = std::make_shared<HddMonitor>(_poller);
    DebugL << "Start monitoring Hdd usage";

    _reader_monitor = std::make_shared<ReaderMonitor>(_poller);
    DebugL << "Start monitoring Stream reader usage";

    _restart_scheduler = std::make_shared<RestartScheduler>(_poller);
    _restart_scheduler->start();
    DebugL << "Start periodical restart scheduler";

    weak_ptr<GlobalMonitor> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(
        300.0f,
        [=]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            auto cpu_usage = strong_self->_cpu_monitor->getCurrentUsage();
            auto mem_usage = strong_self->_mem_monitor->getCurrentUsage();
            auto net_usage = strong_self->_net_monitor->getCurrentUsage();
            auto hdd_usage = strong_self->_hdd_monitor->getCurrentUsage();
            auto reader_usage = strong_self->_reader_monitor->totalReaderCount();

            DebugL << "OS CPU usage: " << format_float_2f(cpu_usage.usagePct) << "%";
            DebugL << "Process CPU usage: " << format_float_2f(cpu_usage.procUsagePct) << "%";
            DebugL << "OS Memory usage: " << format_float_2f(mem_usage.usagePct) << "%";
            DebugL << "Process Memory usage: " << format_float_2f(mem_usage.procUsagePct) << "%";
            DebugL << "Network usage:";
            for (const auto &it : net_usage) {
                DebugL << "     " << it.name << " - in " << format_float_2f(it.rx_mbps) << " Mbps, out " << format_float_2f(it.tx_mbps) << " Mbps";
            }
            DebugL << "HDD usage:";
            for (const auto &it : hdd_usage) {
                DebugL << "     " << it.mount_point << " - total " << format_bytes_human_readable(it.total_bytes) 
                                                    << ", used " << format_bytes_human_readable(it.used_bytes) 
                                                    << ", usage " << format_float_2f(it.usage_pct) << "%";
            }
            DebugL << "Total Reader usage: " << reader_usage;
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

ReaderCountInfoMap GlobalMonitor::getReaderUsage() {
    return _reader_monitor->getCurrentUsage();
}

string GlobalMonitor::getLocalIps() {
    auto net_usage = _net_monitor->getCurrentUsage();
    vector<string> ips;
    for (const auto &net : net_usage) {
        if (!net.ipv4.empty())
            ips.push_back(net.ipv4);
        if (!net.ipv6.empty())
            ips.push_back(net.ipv6);
    }

    _StrPrinter printer;
    for (size_t i = 0; i < ips.size(); i++) {
        printer << ips[i];
        if (i + 1 < ips.size())
            printer << ",";
    }
    return printer;
}

string GlobalMonitor::getMacAddresses() {
    auto net_usage = _net_monitor->getCurrentUsage();
    vector<string> macs;
    for (const auto &net : net_usage) {
        macs.push_back(net.mac_address);
    }

    _StrPrinter printer;
    for (size_t i = 0; i < macs.size(); i++) {
        printer << macs[i];
        if (i + 1 < macs.size())
            printer << ",";
    }
    return printer;
}

void GlobalMonitor::setThreshold(const ResourceType &type, float warning_threshold, float critical_threshold) {
    std::shared_ptr<ResourceMonitor> monitor;
    if (type == ResourceType::CPU && _cpu_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_cpu_monitor);
    }
    if (type == ResourceType::MEMORY && _mem_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_mem_monitor);
    }
    if (type == ResourceType::HDD && _hdd_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_hdd_monitor);
    }
    if (type == ResourceType::NETWORK && _net_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_net_monitor);
    }
    if (type == ResourceType::READER && _reader_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_reader_monitor);
    }
    if (monitor) {
        monitor->setThreshold(warning_threshold, critical_threshold);
    } else {
        WarnL << "Not found " << getResourceTypeString(type) << "monitor. Ignore set threshold";
    }
}

std::pair<float, float> GlobalMonitor::getThreshold(const ResourceType &type) {
    std::shared_ptr<ResourceMonitor> monitor;
    if (type == ResourceType::CPU && _cpu_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_cpu_monitor);
    }
    if (type == ResourceType::MEMORY && _mem_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_mem_monitor);
    }
    if (type == ResourceType::HDD && _hdd_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_hdd_monitor);
    }
    if (type == ResourceType::NETWORK && _net_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_net_monitor);
    }
    if (type == ResourceType::READER && _reader_monitor) {
        monitor = dynamic_pointer_cast<ResourceMonitor>(_reader_monitor);
    }
    if (monitor) {
        return monitor->getThreshold();
    } else {
        WarnL << "Not found " << getResourceTypeString(type) << "monitor. Ignore get threshold";
        return std::make_pair(-1.0, -1.0);
    }
}

void GlobalMonitor::setStreamReaderThreshold(int warning_threshold, int critical_threshold) {
    if (_reader_monitor) {
        _reader_monitor->setStreamReaderThreshold(warning_threshold, critical_threshold);
    }
}

void GlobalMonitor::setStreamReaderCount(const string &camera_id, int reader_count, bool record_stream) {
    if (_reader_monitor) {
        _reader_monitor->setStreamReaderCount(camera_id, reader_count, record_stream);
    }
}

bool GlobalMonitor::isReaderCountLimit(const string &camera_id, bool record_stream) {
    bool ret = false;
    if (_reader_monitor) {
        ret = _reader_monitor->isReaderCountLimit(camera_id, record_stream);
    }
    return ret;
}

void GlobalMonitor::setRestartConfig(const RestartSchedulerConfig &cfg) {
    if (_restart_scheduler) {
        _restart_scheduler->setConfig(cfg);
    }
}

RestartSchedulerConfig GlobalMonitor::getRestartConfig() const {
    if (_restart_scheduler) {
        return _restart_scheduler->getConfig();
    }
    return RestartSchedulerConfig{};
}

} // namespace managerkit