#ifndef S3MANAGERKIT_GLOBALMONITOR_H
#define S3MANAGERKIT_GLOBALMONITOR_H

#include "CpuMonitor.h"
#include "HddMonitor.h"
#include "MemoryMonitor.h"
#include "NetworkMonitor.h"
#include "ReaderMonitor.h"
#include "RestartScheduler.h"
#include "osInfo.h"

extern std::string g_ini_file;

namespace managerkit {

// Forward declaration to avoid pulling Storage/SystemMetrics.h into src/ units
// that include GlobalMonitor.h transitively.
class SystemMetricsImp;

// ──────────────────────────────────────────────────────────────────
// Configuration keys for GlobalMonitor metrics collection & storage
// ──────────────────────────────────────────────────────────────────
namespace GlobalMonitorConfig {
// How long (days) to retain metric samples in the database (default 30)
extern const std::string kMetricsRetentionDays;
// How often (seconds) to collect and store a metric sample (default 10)
extern const std::string kMetricsSampleIntervalSec;
// Maximum number of camera devices the server hardware can handle, based on CPU cores, RAM and NIC speed (default 0)
extern const std::string kMaxAvailableDevicesByHardware;
} // namespace GlobalMonitorConfig

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

    void setThreshold(const ResourceType &type, float warning_threshold = -1, float critical_threshold = -1);

    std::pair<float, float> getThreshold(const ResourceType &type);

    void setStreamReaderThreshold(int warning_threshold = -1, int critical_threshold = -1);
    
    void setStreamReaderCount(const std::string &camera_id, int reader_count, bool record_stream = false);

    bool isReaderCountLimit(const std::string &camera_id, bool record_stream = false);

    ReaderCountInfoMap getReaderUsage();

    int getReaderTotalCount();

    int getReaderTotalCount(const std::string &camera_id);

    bool isReaderCountAvailable(const std::string &camera_id);

    bool isReaderCountAvailable();

    void setRestartConfig(const RestartSchedulerConfig &cfg);

    RestartSchedulerConfig getRestartConfig() const;

    /**
     * Estimate the maximum number of camera devices the server hardware
     * can handle, based on CPU cores, RAM and NIC speed.
     * The result is cached in the ini config file.
     */
    int estimateMaxAvailableDevice();

    /**
     * Build a JSON snapshot of current system resource usage
     * (CPU, RAM, NICs, disks, stream-reader counts, thresholds).
     */
    Json::Value makeSystemStatisticJson();

    /**
     * Query historical metric samples stored in the database.
     * @param from_ts  Start timestamp (unix seconds, inclusive).
     * @param to_ts    End timestamp   (unix seconds, inclusive); 0 = no upper bound.
     * @param limit    Max rows to return; 0 = all.
     * @return JSON array where each element is a SystemMetric row.
     */
    Json::Value getSystemStatisticHistory(int64_t from_ts, int64_t to_ts = 0, int limit = 0);

    /**
     * Find the mount point for a given path.
     * @param path  Path to check (e.g. "/mnt/storage1/videos").
     * @return Mount point (e.g. "/mnt/storage1") or empty string if not found.
     */
    std::string findMountPoint(const std::string& path);

    /**
     * Find the mount point for a given path and return its usage statistics.
     * @param path         Path to check (e.g. "/mnt/storage1/videos").
     * @param usage_pct    Output: usage percentage (0.0 - 100.0).
     * @param used_bytes   Output: used bytes.
     * @param total_bytes  Output: total bytes.
     * @return true if mount point found, false otherwise.
     */
    bool findMountPointUsage(const std::string& path, float &usage_pct, size_t &used_bytes, size_t &total_bytes);

private:
    GlobalMonitor();

private:
    OSInfo _info;
    CpuMonitor::Ptr _cpu_monitor;
    MemoryMonitor::Ptr _mem_monitor;
    NetworkMonitor::Ptr _net_monitor;
    HddMonitor::Ptr _hdd_monitor;
    ReaderMonitor::Ptr _reader_monitor;
    RestartScheduler::Ptr _restart_scheduler;
    std::shared_ptr<SystemMetricsImp> _metrics_store;

    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _metrics_timer;   // periodic sample collection
    toolkit::Timer::Ptr _cleanup_timer;   // periodic old-data purge

    // Runtime statistics
    uint64_t _last_metrics_sample_time = 0; // unix seconds of last sample
    uint64_t _last_cleanup_time = 0;        // unix seconds of last cleanup
    uint64_t _last_debug_log_time = 0;      // unix seconds of last debug log
};

} // namespace managerkit

#endif // S3MANAGERKIT_GLOBALMONITOR_H