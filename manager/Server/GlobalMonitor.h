#ifndef S3MANAGERKIT_GLOBALMONITOR_H
#define S3MANAGERKIT_GLOBALMONITOR_H

#include "CpuMonitor.h"
#include "DiskIoMonitor.h"
#include "HddMonitor.h"
#include "MemoryMonitor.h"
#include "NetworkMonitor.h"
#include "ReaderMonitor.h"
#include "RestartScheduler.h"
#include "StorageWriteMonitor.h"
#include "osInfo.h"

extern std::string g_ini_file;

namespace managerkit {

// Forward declaration to avoid pulling Storage/SystemMetrics.h into src/ units
// that include GlobalMonitor.h transitively.
class SystemMetricsImp;
class SerialTaskQueue;

// ──────────────────────────────────────────────────────────────────
// Configuration keys for GlobalMonitor metrics collection & storage
// ──────────────────────────────────────────────────────────────────
namespace GlobalMonitorConfig {
// How long (days) to retain metric samples in the database (default 30)
extern const std::string kMetricsRetentionDays;
// How often (seconds) to collect and store a metric sample (default 10)
extern const std::string kMetricsSampleIntervalSec;
// How often the process mount inventory is refreshed (default 10 seconds).
extern const std::string kMountRefreshIntervalSec;
// Maximum cached mount snapshot age before an on-demand refresh (default 30 seconds).
extern const std::string kMountSnapshotStaleAfterSec;
// Maximum number of camera devices the server hardware can handle, based on CPU cores, RAM and NIC speed (default 0)
extern const std::string kMaxAvailableDevicesByHardware;
// Default warning/critical thresholds used when the management API omits them
extern const std::string kCpuWarningThreshold;
extern const std::string kCpuCriticalThreshold;
extern const std::string kMemoryWarningThreshold;
extern const std::string kMemoryCriticalThreshold;
extern const std::string kHddWarningThreshold;
extern const std::string kHddCriticalThreshold;
// CPU/RAM must stay at a new alert level for this many 10 s samples before it is reported (default 3)
extern const std::string kSystemAlertSustainSamples;
// CPU/RAM/reader alerts repeat at most this often while still above a threshold (default 300 s)
extern const std::string kSystemAlertRepeatSec;
// While a mount stays above a capacity threshold, repeat its alert at most this often (default 300 s)
extern const std::string kHddAlertRepeatSec;
// Storage IO congestion (DiskIoMonitor). Only mounts holding recordings, tier pools or the DB are watched.
extern const std::string kDiskIoSampleIntervalSec;   // default 5
extern const std::string kDiskIoUtilWarningThreshold;  // % device busy time, default 80
extern const std::string kDiskIoUtilCriticalThreshold; // default 95 (critical also needs await >= critical)
extern const std::string kDiskIoAwaitWarningMs;        // average ms per IO, default 50
extern const std::string kDiskIoAwaitCriticalMs;       // default 200
extern const std::string kIowaitWarningThreshold;      // host % iowait when the kernel has no PSI, default 20
extern const std::string kIowaitCriticalThreshold;     // default 40
extern const std::string kDiskIoPsiFullWarningThreshold;  // host /proc/pressure/io full avg10, default 10
extern const std::string kDiskIoPsiFullCriticalThreshold; // default 30
extern const std::string kDiskIoSustainSamples;        // consecutive samples before a level change, default 3
extern const std::string kDiskIoAlertRepeatSec;        // repeat while still congested, default 300
} // namespace GlobalMonitorConfig

DiskIoConfig loadDiskIoConfig();

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

    DiskIoSnapshot getDiskIoSnapshot();

    // Last write probe of every watched storage directory.
    std::vector<StorageWriteStatus> getStorageWriteStatus();

    /**
     * Watch the IO and writability of `path` (e.g. a tier pool base path) under `key`.
     * An empty path stops watching that key. The record path and the DB directory are always watched.
     */
    void setIoWatchPath(const std::string &key, const std::string &path);

    DiskSnapshot getHddSnapshot(bool refresh_if_stale = false);

    std::string getLocalIps();

    std::string getMacAddresses();

    void setThreshold(const ResourceType &type, float warning_threshold = -1, float critical_threshold = -1);

    std::pair<float, float> getThreshold(const ResourceType &type);

    void setStreamReaderThreshold(int warning_threshold = -1, int critical_threshold = -1);
    
    void setStreamReaderCount(const std::string &camera_id, const std::string &source_id, int reader_count);

    bool isReaderCountLimit(const std::string &camera_id, bool record_stream = false);

    bool isReaderResourceLimit(bool transcode_stream = false);

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
    Json::Value getSystemStatisticHistory(int64_t from_ts, int64_t to_ts = 0, int limit = 0, int bucket_sec = 0);

    /**
     * Find the mount point for a given path.
     * @param path  Path to check (e.g. "/mnt/storage1/videos").
     * @return Mount point (e.g. "/mnt/storage1") or empty string if not found.
     */
    std::string findMountPoint(const std::string& path);

    bool findMountPointInfo(const std::string &path,
                            DiskPartition &out_partition,
                            bool refresh_if_stale = true);

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

    // Directories the server writes: record path (main storage), DB directory, registered tier pools.
    std::vector<std::pair<std::string, bool> > collectStoragePaths();
    std::vector<DiskIoTarget> collectDiskIoTargets();
    std::vector<WriteCheckTarget> collectWriteCheckTargets();
    // "disks" array: capacity of every mount plus io_* fields for watched storage mounts.
    static Json::Value makeDisksJson(const std::vector<DiskPartition> &partitions, const DiskIoSnapshot &io);

private:
    OSInfo _info;
    CpuMonitor::Ptr _cpu_monitor;
    MemoryMonitor::Ptr _mem_monitor;
    NetworkMonitor::Ptr _net_monitor;
    HddMonitor::Ptr _hdd_monitor;
    DiskIoMonitor::Ptr _disk_io_monitor;
    StorageWriteMonitor::Ptr _write_monitor;
    std::mutex _io_watch_mtx;
    std::map<std::string, std::string> _io_watch_paths; // key -> path
    ReaderMonitor::Ptr _reader_monitor;
    RestartScheduler::Ptr _restart_scheduler;
    std::shared_ptr<SystemMetricsImp> _metrics_store;
    // SQLite insert/purge of metric samples, on BlockingExecutors::api(): a locked database waits
    // busy_timeout, which must not stall _poller (a network poller carrying sessions).
    std::shared_ptr<SerialTaskQueue> _metrics_queue;

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
