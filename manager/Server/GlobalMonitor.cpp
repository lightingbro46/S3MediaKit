#include "GlobalMonitor.h"
#include "Common/macros.h"
#include "Common/config.h"
#include "Extension/Benchmark.h"
#include "Local/StatisticRecorder.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Storage/SystemMetrics.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

// ──────────────────────────────────────────────────────────────────
// Config key definitions
// ──────────────────────────────────────────────────────────────────
namespace GlobalMonitorConfig {

#define GM_FIELD "globalmonitor."
const string kMetricsRetentionDays    = GM_FIELD "metrics_retention_days";
const string kMetricsSampleIntervalSec = GM_FIELD "metrics_sample_interval_sec";
const string kMaxAvailableDevicesByHardware = GM_FIELD "max_available_devices_by_hardware";

static onceToken token([]() {
    mINI::Instance()[kMetricsRetentionDays]     = 30;
    mINI::Instance()[kMetricsSampleIntervalSec] = 10;
    mINI::Instance()[kMaxAvailableDevicesByHardware] = 0;
});

} // namespace GlobalMonitorConfig

INSTANCE_IMP(GlobalMonitor)

GlobalMonitor::GlobalMonitor() {
    _poller = EventPollerPool::Instance().getPoller();
    _info = get_os_info();
}

GlobalMonitor::~GlobalMonitor() {
    _metrics_timer.reset();
    _cleanup_timer.reset();
}

void GlobalMonitor::start() {
    if (_metrics_timer && _cleanup_timer) {
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

    _metrics_store = std::make_shared<SystemMetricsImp>();

    // ── Periodic metrics-recording timer ─────────────────────────
    GET_CONFIG(int, sample_interval_sec, GlobalMonitorConfig::kMetricsSampleIntervalSec);
    weak_ptr<GlobalMonitor> weak_self = shared_from_this();
    _metrics_timer = std::make_shared<Timer>(
        (float)sample_interval_sec,
        [=]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            try {
                auto cpu_usage = strong_self->_cpu_monitor->getCurrentUsage();
                auto mem_usage = strong_self->_mem_monitor->getCurrentUsage();
                auto net_usage = strong_self->_net_monitor->getCurrentUsage();
                auto hdd_usage = strong_self->_hdd_monitor->getCurrentUsage();
                auto reader_map = strong_self->_reader_monitor->getCurrentUsage();

                // Build nets JSON
                Json::Value nets_val = Json::arrayValue;
                for (const auto &n : net_usage) {
                    nets_val.append(n.toJson());
                }

                // Build disks JSON
                Json::Value disks_val = Json::arrayValue;
                for (const auto &d : hdd_usage) {
                    disks_val.append(d.toJson());
                }

                int live_count = 0, playback_count = 0;
                for (const auto &kv : reader_map) {
                    live_count     += kv.second.first;
                    playback_count += kv.second.second;
                }

                if (strong_self->_metrics_store) {
                    SystemMetric m;
                    m.timestamp            = (int64_t)time(nullptr);
                    m.cpu_usage_pct        = cpu_usage.usagePct;
                    m.cpu_proc_usage_pct   = cpu_usage.procUsagePct;
                    m.cpu_cores            = cpu_usage.cores;
                    m.ram_used             = (int64_t)mem_usage.usageMemory;
                    m.ram_total            = (int64_t)mem_usage.totalMemory;
                    m.ram_usage_pct        = mem_usage.usagePct;
                    m.reader_total         = live_count + playback_count;
                    m.reader_live          = live_count;
                    m.reader_playback      = playback_count;
                    m.nets_json            = nets_val.toStyledString();
                    m.disks_json           = disks_val.toStyledString();

                    strong_self->_metrics_store->insertMetric(m);
                    TraceL << "System metrics recorded at " << getTimeStr("%Y-%m-%d %H:%M:%S", time(nullptr));
                }
                // Update last metrics sample time
                strong_self->_last_metrics_sample_time = time(nullptr);
                
                if (time(nullptr) - strong_self->_last_debug_log_time >= 300) {
                // ── Periodic debug-log timer (300 s) ──────────────────────────
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
                                                            << ", used "  << format_bytes_human_readable(it.used_bytes)
                                                            << ", usage " << format_float_2f(it.usage_pct) << "%";
                    }
                    DebugL << "Total Reader usage: " << live_count + playback_count;
                    // Update last debug log time
                    strong_self->_last_debug_log_time = time(nullptr);
                }
                
            } catch (const std::exception &ex) {
                WarnL << "Failed to record system metric: " << ex.what();
            }
            return true;
        },
        _poller);

    // ── Daily cleanup timer (every 3600 s) ───────────────────────
    GET_CONFIG(int, retention_days, GlobalMonitorConfig::kMetricsRetentionDays);
    _cleanup_timer = std::make_shared<Timer>(
        3600.0f,
        [=]() {
            auto strong_self = weak_self.lock();
            if (!strong_self || !strong_self->_metrics_store) {
                return false;
            }
            try {
                int64_t cutoff = (int64_t)time(nullptr) - (int64_t)retention_days * 86400;
                strong_self->_metrics_store->deleteOlderThan(cutoff);
                DebugL << "Purged system_metrics older than " << retention_days << " days, cutoff timestamp: " << getTimeStr("%Y-%m-%d %H:%M:%S", cutoff);
                // Update last cleanup time
                strong_self->_last_cleanup_time = time(nullptr);
            } catch (const std::exception &ex) {
                WarnL << "Failed to purge old system metrics: " << ex.what();
            }
            return true;
        },
        _poller);
    DebugL << "GlobalMonitor started (metrics every " << sample_interval_sec<< "s, retention " << retention_days << " days)";
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

int GlobalMonitor::getReaderTotalCount() {
    return _reader_monitor->totalReaderCount();
}

int GlobalMonitor::getReaderTotalCount(const std::string &camera_id) {
    return _reader_monitor->totalReaderCount(camera_id);
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
        WarnL << "Not found " << getResourceTypeString(type) << " monitor. Ignore set threshold";
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
    if (_reader_monitor)
        return _reader_monitor->isReaderCountLimit(camera_id, record_stream);
    return false;
}

bool GlobalMonitor::isReaderCountAvailable(const string &camera_id) {
    if (_reader_monitor)
        return _reader_monitor->isReaderCountAvailable(camera_id);
    return true;
}

bool GlobalMonitor::isReaderCountAvailable() {
    if (_reader_monitor)
        return _reader_monitor->isReaderCountAvailable();
    return true;
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

struct ReaderDeviceStats {
    std::string name;
    std::string ipAddress;
    int total_count    = 0;
    int live_count     = 0;
    int playback_count = 0;
};

Json::Value GlobalMonitor::makeSystemStatisticJson() {
    Json::Value val;
    auto osinfo = getOsInfo();
    val["osInfo"]["platform"]       = osinfo.platform;
    val["osInfo"]["variant"]        = osinfo.variant;
    val["osInfo"]["variant_verison"]= osinfo.variant_version;

    auto cpu_usage = getCpuUsage();
    val["cpu"]["cores"]          = cpu_usage.cores;
    val["cpu"]["usage_pct"]      = sanitize_for_json(cpu_usage.usagePct);
    val["cpu"]["proc_usage_pct"] = sanitize_for_json(cpu_usage.procUsagePct);

    auto mem_usage = getMemUsage();
    val["ram"]["used"]           = (Json::UInt64)mem_usage.usageMemory;
    val["ram"]["total"]          = (Json::UInt64)mem_usage.totalMemory;
    val["ram"]["usage_pct"]      = sanitize_for_json(mem_usage.usagePct);
    val["ram"]["proc_usage_pct"] = sanitize_for_json(mem_usage.procUsagePct);

    val["nets"] = Json::arrayValue;
    for (const auto &n : getNetUsage()) {
        val["nets"].append(n.toJson());
    }

    val["disks"] = Json::arrayValue;
    for (const auto &d : getHddUsage()) {
        val["disks"].append(d.toJson());
    }

    auto reader_map = getReaderUsage();
    int totalReaderCount        = 0;
    int liveReaderCount         = 0;
    int playbackReaderCount     = 0;
    int activeViewingDeviceCount= 0;
    std::vector<ReaderDeviceStats> readerDeviceStatsList;

    for (const auto &item : reader_map) {
        auto recorder = StatisticRecorder::Instance().getRecorder(item.first, false);
        if (recorder) {
            auto params = recorder->getParams();
            ReaderDeviceStats stats;
            stats.name         = params.option.name;
            stats.ipAddress    = params.option.ip;
            stats.live_count     = item.second.first;
            stats.playback_count = item.second.second;
            stats.total_count    = stats.live_count + stats.playback_count;

            liveReaderCount     += stats.live_count;
            playbackReaderCount += stats.playback_count;
            totalReaderCount    += stats.total_count;
            activeViewingDeviceCount += (stats.total_count > 0) ? 1 : 0;

            readerDeviceStatsList.push_back(std::move(stats));
        }
    }

    val["reader"]["totalStreamCount"]        = (Json::UInt)totalReaderCount;
    val["reader"]["liveStreamCount"]         = (Json::UInt)liveReaderCount;
    val["reader"]["playbackStreamCount"]     = (Json::UInt)playbackReaderCount;
    val["reader"]["activeViewingDeviceCount"]= (Json::UInt)activeViewingDeviceCount;
    val["reader"]["devices"] = Json::arrayValue;
    for (const auto &s : readerDeviceStatsList) {
        Json::Value sv;
        sv["name"]               = s.name;
        sv["ipAddress"]          = s.ipAddress;
        sv["liveStreamCount"]    = s.live_count;
        sv["playbackStreamCount"]= s.playback_count;
        sv["totalStreamCount"]   = s.total_count;
        val["reader"]["devices"].append(sv);
    }

    auto cpu_threshold  = getThreshold(ResourceType::CPU);
    val["threshold"]["cpu_levelLow"]    = cpu_threshold.first;
    val["threshold"]["cpu_levelMedium"] = cpu_threshold.second;
    auto mem_threshold  = getThreshold(ResourceType::MEMORY);
    val["threshold"]["ram_levelLow"]    = mem_threshold.first;
    val["threshold"]["ram_levelMedium"] = mem_threshold.second;
    auto hdd_threshold  = getThreshold(ResourceType::HDD);
    val["threshold"]["disk_levelLow"]   = hdd_threshold.first;
    val["threshold"]["disk_levelMedium"]= hdd_threshold.second;
    auto reader_threshold = getThreshold(ResourceType::READER);
    val["threshold"]["reader_levelLow"]   = reader_threshold.first;
    val["threshold"]["reader_levelMedium"]= reader_threshold.second;

    return val;
}

int GlobalMonitor::estimateMaxAvailableDevice() {
    auto &ini = mINI::Instance();
    int maxAvailableDevice = ini[GlobalMonitorConfig::kMaxAvailableDevicesByHardware];
    if (maxAvailableDevice != 0) {
        return maxAvailableDevice;
    }
    auto cpu_usage = getCpuUsage();
    int cpu_core = cpu_usage.cores;
    auto mem_usage = getMemUsage();
    uint64_t mem_cap = mem_usage.totalMemory / 1024 / 1024; // bytes → MB
    auto net_usage = getNetUsage();
    uint64_t net_cap = 0;
    for (const auto &net : net_usage) {
        if (net_cap == 0 || net_cap < net.speed_mbps)
            net_cap = (uint64_t)net.speed_mbps;
    }
    uint64_t disk_cap = 200;
    // compute
    maxAvailableDevice = Benchmark::estimateAvailableDevice(net_cap, disk_cap, cpu_core, mem_cap);
    // persist
    mINI::Instance()[GlobalMonitorConfig::kMaxAvailableDevicesByHardware] = maxAvailableDevice;
    mINI::Instance().dumpFile(g_ini_file);
    return maxAvailableDevice;
}

Json::Value GlobalMonitor::getSystemStatisticHistory(int64_t from_ts, int64_t to_ts, int limit) {
    Json::Value arr = Json::arrayValue;
    if (!_metrics_store) {
        return arr;
    }
    try {
        auto rows = _metrics_store->findByTimeRange(from_ts, to_ts, limit);
        for (const auto &row : rows) {
            arr.append(row.toJson());
        }
    } catch (const std::exception &ex) {
        WarnL << "Failed to query system metrics history: " << ex.what();
    }
    return arr;
}

string GlobalMonitor::findMountPoint(const string& path) {
    auto hdd_usage = getHddUsage();
    string best_match;
    for (const auto& disk : hdd_usage) {
        string mp = disk.mount_point;
        if (start_with(path, mp)) { // path starts with mp
            if (best_match.empty() || mp.size() > best_match.size()) {
                best_match = mp;
            }
        }
    }
    TraceL << "Found mountpoint: " << best_match;
    return best_match;
}

bool GlobalMonitor::findMountPointUsage(const std::string& path, float &usage_pct, size_t &used_bytes, size_t &total_bytes) {
    usage_pct = 0.0f;
    used_bytes = 0;
    total_bytes = 0;
    auto hdd_usage = getHddUsage();
    string best_match;
    for (const auto& disk : hdd_usage) {
        string mp = disk.mount_point;
        if (start_with(path, mp)) { // path starts with mp
            if (best_match.empty() || mp.size() > best_match.size()) {
                best_match = mp;
                usage_pct = disk.usage_pct;
                used_bytes = disk.used_bytes;
                total_bytes = disk.total_bytes;
            }
        }
    }
    TraceL << "Found mountpoint: " << best_match;
    return !best_match.empty();
}

static void* s_tag;

static onceToken g_token(
[]() {
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastPlayerCountChanged, [](BroadcastPlayerCountChangedArgs) {
        auto device_id = args.app;
        bool record_stream = false;
        GET_CONFIG(string, app_name, Record::kAppName);
        if (args.app == app_name) {
            device_id = split(args.stream, "/")[0];
            record_stream = true;
        }
        GlobalMonitor::Instance().setStreamReaderCount(device_id, count, record_stream);
    });
}, 
[]() {
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastPlayerCountChanged);
});

} // namespace managerkit