#ifndef SERVER_DISKIOMONITOR_H
#define SERVER_DISKIOMONITOR_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "json/json.h"
#include "ResourceMonitor.h"

namespace managerkit {

// One row of /proc/diskstats (cumulative counters since boot).
struct DiskIoCounters {
    uint64_t reads = 0;
    uint64_t read_sectors = 0;
    uint64_t read_ms = 0;
    uint64_t writes = 0;
    uint64_t write_sectors = 0;
    uint64_t write_ms = 0;
    uint64_t io_ticks = 0;
    uint64_t weighted_ms = 0;
};

// Everything one sampling pass reads from procfs.
struct DiskIoRawSample {
    int64_t at_ms = 0;                                       // monotonic clock
    std::unordered_map<std::string, DiskIoCounters> disks;   // kernel name ("sdb1", "dm-0") -> counters
    bool has_cpu = false;
    uint64_t cpu_iowait = 0;                                 // /proc/stat jiffies
    uint64_t cpu_total = 0;
    bool has_psi = false;                                    // /proc/pressure/io, kernel >= 4.20
    float psi_some_avg10 = 0.0f;
    float psi_full_avg10 = 0.0f;
};

// A mount that holds recordings, tier pools or the database.
struct DiskIoTarget {
    std::string mount_point;
    std::string device;          // as in /proc/mounts, e.g. "/dev/sdb1", "/dev/mapper/vg-data"
    bool is_main_storage = false; // holds the record path
};

enum class DiskIoLevel : int {
    NORMAL = 0,
    WARNING = 1,
    CRITICAL = 2,
};

const char *diskIoLevelString(DiskIoLevel level);

struct DiskIoConfig {
    float sample_interval_sec = 5.0f;
    float util_warning = 80.0f;
    float util_critical = 95.0f;
    float await_warning_ms = 50.0f;
    float await_critical_ms = 200.0f;
    float iowait_warning = 20.0f;
    float iowait_critical = 40.0f;
    float psi_full_warning = 10.0f;
    float psi_full_critical = 30.0f;
    int sustain_samples = 3;
    int alert_repeat_sec = 300;
};

struct MountIoStats {
    std::string mount_point;
    std::string device;
    std::string kernel_name;
    bool is_main_storage = false;
    bool valid = false;          // false until two consecutive samples exist for the device
    float util_pct = 0.0f;
    float await_ms = 0.0f;
    float queue_depth = 0.0f;
    float read_mbps = 0.0f;
    float write_mbps = 0.0f;
    DiskIoLevel state = DiskIoLevel::NORMAL; // confirmed (sustained) level

    // Adds the io_* fields to one element of the "disks" JSON array.
    void appendJson(Json::Value &disk) const;
};

struct DiskIoSnapshot {
    std::vector<MountIoStats> mounts;
    bool has_iowait = false;
    float iowait_pct = 0.0f;
    bool has_psi = false;
    float psi_some_pct = 0.0f;
    float psi_full_pct = 0.0f;
    DiskIoLevel system_state = DiskIoLevel::NORMAL;
    int64_t sampled_at = 0;      // unix seconds
    bool valid = false;

    // {"iowait_pct", "psi_io_some_pct", "psi_io_full_pct", "state"}; absent sources are null.
    Json::Value systemJson() const;
    const MountIoStats *findMount(const std::string &mount_point) const;
};

/**
 * Samples /proc/diskstats, /proc/stat and /proc/pressure/io on a timer and raises
 * kBroadcastDiskIoAlert when a storage mount (or the host) stays congested.
 * Reading procfs is not disk IO, so the timer runs on the poller.
 */
class DiskIoMonitor : public ResourceMonitor,
                      public std::enable_shared_from_this<DiskIoMonitor> {
public:
    using Ptr = std::shared_ptr<DiskIoMonitor>;
    using SampleProvider = std::function<bool(DiskIoRawSample &)>;
    using TargetProvider = std::function<std::vector<DiskIoTarget>()>;
    // Maps a /proc/mounts device to its /proc/diskstats name; empty when unknown.
    using DeviceResolver = std::function<std::string(const std::string &device)>;
    using ConfigProvider = std::function<DiskIoConfig()>;

    struct Providers {
        SampleProvider sample;
        TargetProvider targets;
        DeviceResolver resolver;
        ConfigProvider config;
    };

    explicit DiskIoMonitor(toolkit::EventPoller::Ptr poller, const Providers &providers);
    ~DiskIoMonitor() override;

    void start() override;

    // One sampling pass; the timer calls it, tests call it directly.
    bool sampleNow();

    DiskIoSnapshot getSnapshot();

    static bool readProcSample(DiskIoRawSample &out);
    static bool parseDiskStats(const std::string &text, std::unordered_map<std::string, DiskIoCounters> &out);
    static bool parseCpuStat(const std::string &text, uint64_t &iowait, uint64_t &total);
    static bool parsePressure(const std::string &text, float &some_avg10, float &full_avg10);
    static std::string resolveKernelName(const std::string &device);
    // "sdb1" -> "sdb", "nvme0n1p2" -> "nvme0n1"; empty when it is not a partition name.
    static std::string parentDiskName(const std::string &name);

private:
    struct AlertState {
        AlertLevelTracker tracker;
        std::string last_metric = "util";
    };

    static bool advance(AlertState &st, DiskIoLevel observed, int64_t now_ms, const DiskIoConfig &cfg);

private:
    Providers _providers;
    toolkit::Timer::Ptr _timer;
    DiskIoRawSample _prev;
    bool _has_prev = false;
    std::map<std::string, AlertState> _alerts; // mount point, "" = host level
    DiskIoSnapshot _snapshot;
};

} // namespace managerkit

#endif // SERVER_DISKIOMONITOR_H
