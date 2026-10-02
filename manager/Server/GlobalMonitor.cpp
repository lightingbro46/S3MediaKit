#include "GlobalMonitor.h"
#include "Common/macros.h"
#include "Common/config.h"
#include "Extension/Benchmark.h"
#include "Local/StatisticRecorder.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Storage/SystemMetrics.h"
#include "Storage/DbStorage.h"
#include "Util/File.h"
#include "Common/BlockingExecutors.h"
#include "Common/SerialTaskQueue.h"
#include <algorithm>
#include <map>

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
const string kMountRefreshIntervalSec = GM_FIELD "mount_refresh_interval_seconds";
const string kMountSnapshotStaleAfterSec = GM_FIELD "mount_snapshot_stale_after_seconds";
const string kMaxAvailableDevicesByHardware = GM_FIELD "max_available_devices_by_hardware";
const string kCpuWarningThreshold      = GM_FIELD "cpu_warning_threshold";
const string kCpuCriticalThreshold     = GM_FIELD "cpu_critical_threshold";
const string kMemoryWarningThreshold   = GM_FIELD "memory_warning_threshold";
const string kMemoryCriticalThreshold  = GM_FIELD "memory_critical_threshold";
const string kHddWarningThreshold      = GM_FIELD "hdd_warning_threshold";
const string kHddCriticalThreshold     = GM_FIELD "hdd_critical_threshold";
const string kHddAlertRepeatSec        = GM_FIELD "hdd_alert_repeat_sec";
const string kSystemAlertSustainSamples = GM_FIELD "system_alert_sustain_samples";
const string kSystemAlertRepeatSec     = GM_FIELD "system_alert_repeat_sec";
const string kDiskIoSampleIntervalSec  = GM_FIELD "disk_io_sample_interval_sec";
const string kDiskIoUtilWarningThreshold  = GM_FIELD "disk_io_util_warning_threshold";
const string kDiskIoUtilCriticalThreshold = GM_FIELD "disk_io_util_critical_threshold";
const string kDiskIoAwaitWarningMs     = GM_FIELD "disk_io_await_warning_ms";
const string kDiskIoAwaitCriticalMs    = GM_FIELD "disk_io_await_critical_ms";
const string kIowaitWarningThreshold   = GM_FIELD "iowait_warning_threshold";
const string kIowaitCriticalThreshold  = GM_FIELD "iowait_critical_threshold";
const string kDiskIoPsiFullWarningThreshold  = GM_FIELD "disk_io_psi_full_warning_threshold";
const string kDiskIoPsiFullCriticalThreshold = GM_FIELD "disk_io_psi_full_critical_threshold";
const string kDiskIoSustainSamples     = GM_FIELD "disk_io_sustain_samples";
const string kDiskIoAlertRepeatSec     = GM_FIELD "disk_io_alert_repeat_sec";

static onceToken token([]() {
    mINI::Instance()[kMetricsRetentionDays]     = 7;
    mINI::Instance()[kMetricsSampleIntervalSec] = 10;
    mINI::Instance()[kMountRefreshIntervalSec] = 10;
    mINI::Instance()[kMountSnapshotStaleAfterSec] = 30;
    mINI::Instance()[kMaxAvailableDevicesByHardware] = 0;
    mINI::Instance()[kCpuWarningThreshold]      = 80.0;
    mINI::Instance()[kCpuCriticalThreshold]     = 90.0;
    mINI::Instance()[kMemoryWarningThreshold]   = 80.0;
    mINI::Instance()[kMemoryCriticalThreshold]  = 90.0;
    mINI::Instance()[kHddWarningThreshold]      = 80.0;
    mINI::Instance()[kHddCriticalThreshold]     = 90.0;
    mINI::Instance()[kHddAlertRepeatSec]        = 300;
    mINI::Instance()[kSystemAlertSustainSamples] = 3;
    mINI::Instance()[kSystemAlertRepeatSec]     = 300;
    mINI::Instance()[kDiskIoSampleIntervalSec]  = 5;
    mINI::Instance()[kDiskIoUtilWarningThreshold]  = 80.0;
    mINI::Instance()[kDiskIoUtilCriticalThreshold] = 95.0;
    mINI::Instance()[kDiskIoAwaitWarningMs]     = 50.0;
    mINI::Instance()[kDiskIoAwaitCriticalMs]    = 200.0;
    mINI::Instance()[kIowaitWarningThreshold]   = 20.0;
    mINI::Instance()[kIowaitCriticalThreshold]  = 40.0;
    mINI::Instance()[kDiskIoPsiFullWarningThreshold]  = 10.0;
    mINI::Instance()[kDiskIoPsiFullCriticalThreshold] = 30.0;
    mINI::Instance()[kDiskIoSustainSamples]     = 3;
    mINI::Instance()[kDiskIoAlertRepeatSec]     = 300;
});

} // namespace GlobalMonitorConfig

DiskIoConfig loadDiskIoConfig() {
    GET_CONFIG(float, sample_interval_sec, GlobalMonitorConfig::kDiskIoSampleIntervalSec)
    GET_CONFIG(float, util_warning, GlobalMonitorConfig::kDiskIoUtilWarningThreshold)
    GET_CONFIG(float, util_critical, GlobalMonitorConfig::kDiskIoUtilCriticalThreshold)
    GET_CONFIG(float, await_warning_ms, GlobalMonitorConfig::kDiskIoAwaitWarningMs)
    GET_CONFIG(float, await_critical_ms, GlobalMonitorConfig::kDiskIoAwaitCriticalMs)
    GET_CONFIG(float, iowait_warning, GlobalMonitorConfig::kIowaitWarningThreshold)
    GET_CONFIG(float, iowait_critical, GlobalMonitorConfig::kIowaitCriticalThreshold)
    GET_CONFIG(float, psi_full_warning, GlobalMonitorConfig::kDiskIoPsiFullWarningThreshold)
    GET_CONFIG(float, psi_full_critical, GlobalMonitorConfig::kDiskIoPsiFullCriticalThreshold)
    GET_CONFIG(int, sustain_samples, GlobalMonitorConfig::kDiskIoSustainSamples)
    GET_CONFIG(int, alert_repeat_sec, GlobalMonitorConfig::kDiskIoAlertRepeatSec)
    DiskIoConfig cfg;
    cfg.sample_interval_sec = sample_interval_sec;
    cfg.util_warning = util_warning;
    cfg.util_critical = util_critical;
    cfg.await_warning_ms = await_warning_ms;
    cfg.await_critical_ms = await_critical_ms;
    cfg.iowait_warning = iowait_warning;
    cfg.iowait_critical = iowait_critical;
    cfg.psi_full_warning = psi_full_warning;
    cfg.psi_full_critical = psi_full_critical;
    cfg.sustain_samples = sustain_samples;
    cfg.alert_repeat_sec = alert_repeat_sec;
    return cfg;
}

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

    GET_CONFIG(int, alert_sustain_samples, GlobalMonitorConfig::kSystemAlertSustainSamples)
    GET_CONFIG(int, alert_repeat_sec, GlobalMonitorConfig::kSystemAlertRepeatSec)

    _cpu_monitor = std::make_shared<CpuMonitor>(_poller);
    _cpu_monitor->setAlertPolicy(alert_sustain_samples, alert_repeat_sec);
    GET_CONFIG(float, default_warning_cpu_threshold, GlobalMonitorConfig::kCpuWarningThreshold)
    GET_CONFIG(float, default_critical_cpu_threshold, GlobalMonitorConfig::kCpuCriticalThreshold)
    _cpu_monitor->setThreshold(default_warning_cpu_threshold, default_critical_cpu_threshold);
    DebugL << "Start monitoring CPU usage";

    _mem_monitor = std::make_shared<MemoryMonitor>(_poller);
    GET_CONFIG(float, default_warning_mem_threshold, GlobalMonitorConfig::kMemoryWarningThreshold)
    GET_CONFIG(float, default_critical_mem_threshold, GlobalMonitorConfig::kMemoryCriticalThreshold)
    _mem_monitor->setThreshold(default_warning_mem_threshold, default_critical_mem_threshold);
    _mem_monitor->setAlertPolicy(alert_sustain_samples, alert_repeat_sec);
    DebugL << "Start monitoring Memory usage";

    _net_monitor = std::make_shared<NetworkMonitor>(_poller);
    // Note: Do not set network threshold
    DebugL << "Start monitoring Network usage";

    GET_CONFIG(float, mount_refresh_interval_sec, GlobalMonitorConfig::kMountRefreshIntervalSec)
    if (mount_refresh_interval_sec <= 0) {
        mount_refresh_interval_sec = 10.0f;
    }
    _hdd_monitor = std::make_shared<HddMonitor>(_poller, mount_refresh_interval_sec);
    _hdd_monitor->start();
    GET_CONFIG(float, default_warning_hdd_threshold, GlobalMonitorConfig::kHddWarningThreshold)
    GET_CONFIG(float, default_critical_hdd_threshold, GlobalMonitorConfig::kHddCriticalThreshold)
    _hdd_monitor->setThreshold(default_warning_hdd_threshold, default_critical_hdd_threshold);
    GET_CONFIG(int, hdd_alert_repeat_sec, GlobalMonitorConfig::kHddAlertRepeatSec)
    _hdd_monitor->setAlertRepeatSec(hdd_alert_repeat_sec);
    DebugL << "Start monitoring Hdd usage";

    {
        DiskIoMonitor::Providers providers;
        providers.config = loadDiskIoConfig;
        // Singleton: GlobalMonitor outlives its monitors, so no weak_ptr needed.
        providers.targets = []() { return GlobalMonitor::Instance().collectDiskIoTargets(); };
        _disk_io_monitor = std::make_shared<DiskIoMonitor>(_poller, providers);
        _disk_io_monitor->start();
        DebugL << "Start monitoring storage IO";

        // Write probes follow the mount refresh cadence; repeat alerts like the capacity ones.
        _write_monitor = std::make_shared<StorageWriteMonitor>(
            _poller, mount_refresh_interval_sec,
            []() { return GlobalMonitor::Instance().collectWriteCheckTargets(); },
            StorageWriteMonitor::Probe(), nullptr,
            []() {
                GET_CONFIG(int, repeat_sec, GlobalMonitorConfig::kHddAlertRepeatSec)
                return repeat_sec;
            });
        _write_monitor->start();
        DebugL << "Start monitoring storage writability";
    }

    _reader_monitor = std::make_shared<ReaderMonitor>(_poller);
    // Reader counts change on events, not samples: no sustain.
    _reader_monitor->setAlertPolicy(1, alert_repeat_sec);
    DebugL << "Start monitoring Stream reader usage";

    _restart_scheduler = std::make_shared<RestartScheduler>(_poller);
    _restart_scheduler->start();
    DebugL << "Start periodical restart scheduler";

    _metrics_store = std::make_shared<SystemMetricsImp>();
    _metrics_queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::api(), "system metrics");

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
                auto disk_io = strong_self->getDiskIoSnapshot();
                auto reader_map = strong_self->_reader_monitor->getCurrentUsage();

                // Build nets JSON
                Json::Value nets_val = Json::arrayValue;
                for (const auto &n : net_usage) {
                    nets_val.append(n.toJson());
                }

                // Build disks JSON
                Json::Value disks_val = makeDisksJson(hdd_usage, disk_io);

                int live_count = 0, playback_count = 0;
                for (const auto &kv : reader_map) {
                    live_count     += kv.second.first;
                    playback_count += kv.second.second;
                }

                // A previous insert still waiting on a locked database: drop this sample rather
                // than pile up writes.
                if (strong_self->_metrics_store && strong_self->_metrics_queue->pending() == 0) {
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

                    auto store = strong_self->_metrics_store;
                    strong_self->_metrics_queue->post([store, m]() {
                        try {
                            store->insertMetric(m);
                            DebugL << "System metrics recorded at " << getTimeStr("%Y-%m-%d %H:%M:%S", m.timestamp);
                        } catch (const std::exception &ex) {
                            WarnL << "Failed to record system metric: " << ex.what();
                        }
                    });
                }
                // Update last metrics sample time
                strong_self->_last_metrics_sample_time = time(nullptr);
                
                if (time(nullptr) - strong_self->_last_debug_log_time >= 300) {
                // ── Periodic debug-log timer (300 s) ──────────────────────────
                    InfoL << "OS CPU usage: " << format_float_2f(cpu_usage.usagePct) << "%";
                    InfoL << "Process CPU usage: " << format_float_2f(cpu_usage.procUsagePct) << "%";
                    InfoL << "OS Memory usage: " << format_float_2f(mem_usage.usagePct) << "%";
                    InfoL << "Process Memory usage: " << format_float_2f(mem_usage.procUsagePct) << "%";
                    InfoL << "Network usage:";
                    for (const auto &it : net_usage) {
                        InfoL << "     " << it.name << " - in " << format_float_2f(it.rx_mbps) << " Mbps, out " << format_float_2f(it.tx_mbps) << " Mbps";
                    }
                    InfoL << "HDD usage:";
                    for (const auto &it : hdd_usage) {
                        InfoL << "     " << it.mount_point << " - total " << format_bytes_human_readable(it.total_bytes)
                                                            << ", used "  << format_bytes_human_readable(it.used_bytes)
                                                            << ", usage " << format_float_2f(it.usage_pct) << "%";
                    }
                    InfoL << "Total Reader usage: " << live_count + playback_count;
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
            int64_t cutoff = (int64_t)time(nullptr) - (int64_t)retention_days * 86400;
            auto store = strong_self->_metrics_store;
            strong_self->_metrics_queue->post([store, cutoff]() {
                try {
                    store->deleteOlderThan(cutoff);
                    DebugL << "Purged system_metrics older than " << retention_days << " days, cutoff timestamp: " << getTimeStr("%Y-%m-%d %H:%M:%S", cutoff);
                } catch (const std::exception &ex) {
                    WarnL << "Failed to purge old system metrics: " << ex.what();
                }
            });
            // Update last cleanup time
            strong_self->_last_cleanup_time = time(nullptr);
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
    return getHddSnapshot(false).partitions;
}

DiskSnapshot GlobalMonitor::getHddSnapshot(bool refresh_if_stale) {
    if (!_hdd_monitor) {
        return DiskSnapshot();
    }
    auto snapshot = _hdd_monitor->getSnapshot();
    if (!refresh_if_stale) {
        return snapshot;
    }
    GET_CONFIG(int64_t, stale_after_sec, GlobalMonitorConfig::kMountSnapshotStaleAfterSec)
    if (stale_after_sec <= 0) {
        stale_after_sec = 30;
    }
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (!snapshot.valid || snapshot.refreshed_at <= 0 ||
        now - snapshot.refreshed_at > stale_after_sec) {
        if (!_hdd_monitor->refreshNow()) {
            return DiskSnapshot();
        }
        snapshot = _hdd_monitor->getSnapshot();
    }
    return snapshot;
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

void GlobalMonitor::setStreamReaderCount(const string &camera_id, const string &source_id, int reader_count) {
    if (_reader_monitor) {
        _reader_monitor->setStreamReaderCount(camera_id, source_id, reader_count);
    }
}

bool GlobalMonitor::isReaderResourceLimit(bool transcode_stream) {
    const auto threshold_for = [transcode_stream](const std::pair<float, float> &threshold) {
        // Transcoding needs more headroom, so reject at the warning level;
        // ordinary playback is rejected only at the critical level.
        return transcode_stream ? threshold.first : threshold.second;
    };

    if (_cpu_monitor) {
        auto threshold = threshold_for(getThreshold(ResourceType::CPU));
        auto usage = _cpu_monitor->getCurrentUsage().usagePct;
        if (threshold > 0 && usage >= threshold) {
            WarnL << "Reject reader because CPU usage " << usage << "% reached " << threshold
                  << "% (transcode=" << transcode_stream << ")";
            return true;
        }
    }

    if (_mem_monitor) {
        auto threshold = threshold_for(getThreshold(ResourceType::MEMORY));
        auto usage = _mem_monitor->getCurrentUsage().usagePct;
        if (threshold > 0 && usage >= threshold) {
            WarnL << "Reject reader because memory usage " << usage << "% reached " << threshold
                  << "% (transcode=" << transcode_stream << ")";
            return true;
        }
    }

    return false;
}

bool GlobalMonitor::isReaderCountLimit(const string &camera_id, bool record_stream) {
    if (_reader_monitor && _reader_monitor->isReaderCountLimit(camera_id, record_stream)) {
        return true;
    }
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

    const auto disk_io = getDiskIoSnapshot();
    val["disks"] = makeDisksJson(getHddUsage(), disk_io);
    val["disk_io"] = disk_io.systemJson();
    val["storage_write"] = Json::arrayValue;
    for (const auto &w : getStorageWriteStatus()) {
        val["storage_write"].append(w.toJson());
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

static float jsonValueToFloat(const Json::Value &v) {
    if (v.isNumeric()) {
        return v.asFloat();
    }
    if (v.isString()) {
        try {
            return std::stof(v.asString());
        } catch (...) {
            return 0.0f;
        }
    }
    return 0.0f;
}

static void appendDerivedMetricFields(Json::Value &v) {
    float net_rx_mbps = 0.0f;
    float net_tx_mbps = 0.0f;
    if (v["nets"].isArray()) {
        for (const auto &n : v["nets"]) {
            net_rx_mbps += jsonValueToFloat(n["rx_mbps"]);
            net_tx_mbps += jsonValueToFloat(n["tx_mbps"]);
        }
    }

    float hdd_usage_pct = 0.0f;
    // Worst watched storage mount; null fields (no IO sample yet, or rows from before DiskIoMonitor) count as 0.
    float disk_io_util_pct = 0.0f;
    float disk_await_ms = 0.0f;
    if (v["disks"].isArray()) {
        for (const auto &d : v["disks"]) {
            hdd_usage_pct = std::max(hdd_usage_pct, jsonValueToFloat(d["used_pct"]));
            disk_io_util_pct = std::max(disk_io_util_pct, jsonValueToFloat(d["io_util_pct"]));
            disk_await_ms = std::max(disk_await_ms, jsonValueToFloat(d["await_ms"]));
        }
    }

    v["net_rx_mbps"] = net_rx_mbps;
    v["net_tx_mbps"] = net_tx_mbps;
    v["hdd_usage_pct"] = hdd_usage_pct;
    v["disk_io_util_pct"] = disk_io_util_pct;
    v["disk_await_ms"] = disk_await_ms;
}

struct SystemMetricBucket {
    int64_t timestamp = 0;
    int count = 0;
    double cpu_usage_pct = 0;
    double cpu_proc_usage_pct = 0;
    double ram_used = 0;
    double ram_total = 0;
    double ram_usage_pct = 0;
    double reader_total = 0;
    double reader_live = 0;
    double reader_playback = 0;
    double net_rx_mbps = 0;
    double net_tx_mbps = 0;
    double hdd_usage_pct = 0;
    double disk_io_util_pct = 0;
    double disk_await_ms = 0;
};

Json::Value GlobalMonitor::getSystemStatisticHistory(int64_t from_ts, int64_t to_ts, int limit, int bucket_sec) {
    Json::Value arr = Json::arrayValue;
    auto make_live_fallback = [&]() -> Json::Value {
        Json::Value v;
        int64_t now = (int64_t)time(nullptr);
        if (from_ts > now || (to_ts > 0 && now > to_ts)) {
            return v;
        }
        if (!_cpu_monitor || !_mem_monitor || !_net_monitor || !_hdd_monitor) {
            return v;
        }

        auto cpu_usage = _cpu_monitor->getCurrentUsage();
        auto mem_usage = _mem_monitor->getCurrentUsage();
        auto net_usage = _net_monitor->getCurrentUsage();
        auto hdd_usage = _hdd_monitor->getCurrentUsage();

        int live_count = 0;
        int playback_count = 0;
        if (_reader_monitor) {
            auto reader_map = _reader_monitor->getCurrentUsage();
            for (const auto &kv : reader_map) {
                live_count += kv.second.first;
                playback_count += kv.second.second;
            }
        }

        v["timestamp"] = (Json::Int64)now;
        v["samples"] = 1;
        v["history_source"] = "live_fallback";
        v["cpu_usage_pct"] = cpu_usage.usagePct;
        v["cpu_proc_usage_pct"] = cpu_usage.procUsagePct;
        v["cpu_cores"] = cpu_usage.cores;
        v["ram_used"] = (Json::UInt64)mem_usage.usageMemory;
        v["ram_total"] = (Json::UInt64)mem_usage.totalMemory;
        v["ram_usage_pct"] = mem_usage.usagePct;
        v["reader_total"] = live_count + playback_count;
        v["reader_live"] = live_count;
        v["reader_playback"] = playback_count;

        v["nets"] = Json::arrayValue;
        for (const auto &n : net_usage) {
            v["nets"].append(n.toJson());
        }
        v["disks"] = makeDisksJson(hdd_usage, getDiskIoSnapshot());
        appendDerivedMetricFields(v);
        if (bucket_sec > 0) {
            v["timestamp"] = (Json::Int64)((now / bucket_sec) * bucket_sec);
        }
        return v;
    };

    auto append_live_fallback = [&]() {
        if (limit == 0 || limit > 0) {
            auto live = make_live_fallback();
            if (!live.isNull()) {
                arr.append(live);
            }
        }
    };

    if (!_metrics_store) {
        append_live_fallback();
        return arr;
    }
    try {
        auto rows = _metrics_store->findByTimeRange(from_ts, to_ts, limit);
        if (rows.empty()) {
            append_live_fallback();
            return arr;
        }
        if (bucket_sec <= 0) {
            for (const auto &row : rows) {
                auto v = row.toJson();
                appendDerivedMetricFields(v);
                arr.append(v);
            }
            return arr;
        }

        std::map<int64_t, SystemMetricBucket> buckets;
        for (const auto &row : rows) {
            auto v = row.toJson();
            appendDerivedMetricFields(v);

            int64_t bucket_ts = (row.timestamp / bucket_sec) * bucket_sec;
            auto &b = buckets[bucket_ts];
            b.timestamp = bucket_ts;
            b.count++;
            b.cpu_usage_pct += row.cpu_usage_pct;
            b.cpu_proc_usage_pct += row.cpu_proc_usage_pct;
            b.ram_used += row.ram_used;
            b.ram_total += row.ram_total;
            b.ram_usage_pct += row.ram_usage_pct;
            b.reader_total += row.reader_total;
            b.reader_live += row.reader_live;
            b.reader_playback += row.reader_playback;
            b.net_rx_mbps += jsonValueToFloat(v["net_rx_mbps"]);
            b.net_tx_mbps += jsonValueToFloat(v["net_tx_mbps"]);
            b.hdd_usage_pct += jsonValueToFloat(v["hdd_usage_pct"]);
            b.disk_io_util_pct += jsonValueToFloat(v["disk_io_util_pct"]);
            b.disk_await_ms += jsonValueToFloat(v["disk_await_ms"]);
        }

        for (const auto &kv : buckets) {
            const auto &b = kv.second;
            if (b.count <= 0) {
                continue;
            }
            Json::Value v;
            v["timestamp"] = (Json::Int64)b.timestamp;
            v["samples"] = b.count;
            v["cpu_usage_pct"] = b.cpu_usage_pct / b.count;
            v["cpu_proc_usage_pct"] = b.cpu_proc_usage_pct / b.count;
            v["ram_used"] = (Json::UInt64)(b.ram_used / b.count);
            v["ram_total"] = (Json::UInt64)(b.ram_total / b.count);
            v["ram_usage_pct"] = b.ram_usage_pct / b.count;
            v["reader_total"] = (int)(b.reader_total / b.count + 0.5);
            v["reader_live"] = (int)(b.reader_live / b.count + 0.5);
            v["reader_playback"] = (int)(b.reader_playback / b.count + 0.5);
            v["net_rx_mbps"] = b.net_rx_mbps / b.count;
            v["net_tx_mbps"] = b.net_tx_mbps / b.count;
            v["hdd_usage_pct"] = b.hdd_usage_pct / b.count;
            v["disk_io_util_pct"] = b.disk_io_util_pct / b.count;
            v["disk_await_ms"] = b.disk_await_ms / b.count;
            arr.append(v);
        }
    } catch (const std::exception &ex) {
        WarnL << "Failed to query system metrics history: " << ex.what();
    }
    return arr;
}

DiskIoSnapshot GlobalMonitor::getDiskIoSnapshot() {
    if (!_disk_io_monitor) {
        return DiskIoSnapshot();
    }
    return _disk_io_monitor->getSnapshot();
}

void GlobalMonitor::setIoWatchPath(const std::string &key, const std::string &path) {
    lock_guard<mutex> lck(_io_watch_mtx);
    if (path.empty()) {
        _io_watch_paths.erase(key);
    } else {
        _io_watch_paths[key] = path;
    }
}

std::vector<StorageWriteStatus> GlobalMonitor::getStorageWriteStatus() {
    if (!_write_monitor) {
        return std::vector<StorageWriteStatus>();
    }
    return _write_monitor->getStatus();
}

std::vector<std::pair<string, bool> > GlobalMonitor::collectStoragePaths() {
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    GET_CONFIG(string, db_save_path, Database::kDbSavePath);
    // path -> holds the record path
    std::vector<std::pair<string, bool> > paths;
    paths.emplace_back(File::absolutePath(app_name, mp4_save_path), true);
    paths.emplace_back(File::absolutePath("", db_save_path), false);
    {
        lock_guard<mutex> lck(_io_watch_mtx);
        for (const auto &kv : _io_watch_paths) {
            paths.emplace_back(kv.second, false);
        }
    }
    return paths;
}

std::vector<WriteCheckTarget> GlobalMonitor::collectWriteCheckTargets() {
    const auto snapshot = getHddSnapshot(false);
    std::vector<WriteCheckTarget> targets;
    for (const auto &p : collectStoragePaths()) {
        if (std::any_of(targets.begin(), targets.end(), [&](const WriteCheckTarget &t) { return t.path == p.first; })) {
            continue;
        }
        WriteCheckTarget target;
        target.path = p.first;
        target.is_main_storage = p.second;
        DiskPartition partition;
        if (HddMonitor::findMountPoint(snapshot, p.first, partition)) {
            target.mount_point = partition.mount_point;
            target.device = partition.device;
            target.read_only = partition.read_only;
        }
        targets.push_back(target);
    }
    return targets;
}

std::vector<DiskIoTarget> GlobalMonitor::collectDiskIoTargets() {
    const auto paths = collectStoragePaths();
    // Cached snapshot only: this runs on the poller and must never statvfs.
    const auto snapshot = getHddSnapshot(false);
    std::vector<DiskIoTarget> targets;
    for (const auto &p : paths) {
        DiskPartition partition;
        if (!HddMonitor::findMountPoint(snapshot, p.first, partition)) {
            continue;
        }
        // NFS/CIFS have no block device in /proc/diskstats.
        if (partition.isNetworkFileSystem() || partition.device.compare(0, 5, "/dev/") != 0) {
            continue;
        }
        auto it = std::find_if(targets.begin(), targets.end(), [&](const DiskIoTarget &t) {
            return t.mount_point == partition.mount_point;
        });
        if (it != targets.end()) {
            it->is_main_storage = it->is_main_storage || p.second;
            continue;
        }
        DiskIoTarget target;
        target.mount_point = partition.mount_point;
        target.device = partition.device;
        target.is_main_storage = p.second;
        targets.push_back(target);
    }
    return targets;
}

Json::Value GlobalMonitor::makeDisksJson(const std::vector<DiskPartition> &partitions, const DiskIoSnapshot &io) {
    Json::Value disks = Json::arrayValue;
    for (const auto &d : partitions) {
        auto dv = d.toJson();
        if (const auto *stats = io.findMount(d.mount_point)) {
            stats->appendJson(dv);
        }
        disks.append(dv);
    }
    return disks;
}

string GlobalMonitor::findMountPoint(const string& path) {
    DiskPartition partition;
    const bool found = findMountPointInfo(path, partition, false);
    TraceL << "Found mountpoint: " << (found ? partition.mount_point : "");
    return found ? partition.mount_point : "";
}

bool GlobalMonitor::findMountPointInfo(const string &path,
                                       DiskPartition &out_partition,
                                       bool refresh_if_stale) {
    return HddMonitor::findMountPoint(
        getHddSnapshot(refresh_if_stale), path, out_partition);
}

bool GlobalMonitor::findMountPointUsage(const std::string& path, float &usage_pct, size_t &used_bytes, size_t &total_bytes) {
    usage_pct = 0.0f;
    used_bytes = 0;
    total_bytes = 0;
    DiskPartition partition;
    if (!findMountPointInfo(path, partition, false)) {
        TraceL << "Found mountpoint: ";
        return false;
    }
    usage_pct = partition.usage_pct;
    used_bytes = partition.used_bytes;
    total_bytes = partition.total_bytes;
    TraceL << "Found mountpoint: " << partition.mount_point;
    return true;
}

static void* s_tag;

static onceToken g_token(
[]() {
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastPlayerCountChanged, [](BroadcastPlayerCountChangedArgs) {
        DebugL << "Player count changed: " << args.shortUrl() << ", size: " << count;
        auto device_id = args.app;
        bool record_stream = false;
        GET_CONFIG(string, app_name, Record::kAppName);
        if (args.app == app_name) {
            device_id = split(args.stream, "/")[0];
            record_stream = true;
        }
        // The event count is already the total reader count of the source
        // muxer across all protocols. Do not include a protocol/schema in the
        // key, otherwise the same readers would be counted more than once.
        // Replay sources may use a different timestamp in `stream`. Keep it
        // in the key so concurrent replays remain separate, while
        // ReaderMonitor aggregates all record sources by camera_id.
        auto source_id = device_id + "|" + (record_stream ? "record:" : "live:") + args.shortUrl();
        GlobalMonitor::Instance().setStreamReaderCount(device_id, source_id, count);
    });
}, 
[]() {
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastPlayerCountChanged);
});

} // namespace managerkit
