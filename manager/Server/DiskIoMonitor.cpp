#if defined(__linux__)
#include <dirent.h>
#include <climits>
#include <cstdlib>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include "Util/NoticeCenter.h"
#include "Util/util.h"
#include "DiskIoMonitor.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

static const char kHostAlertKey[] = "";

const char *diskIoLevelString(DiskIoLevel level) {
    switch (level) {
        case DiskIoLevel::WARNING: return "warning";
        case DiskIoLevel::CRITICAL: return "critical";
        default: return "normal";
    }
}

void MountIoStats::appendJson(Json::Value &disk) const {
    if (valid) {
        disk["io_util_pct"] = sanitize_for_json(util_pct);
        disk["await_ms"] = sanitize_for_json(await_ms);
        disk["io_queue_depth"] = sanitize_for_json(queue_depth);
        disk["read_mbps"] = sanitize_for_json(read_mbps);
        disk["write_mbps"] = sanitize_for_json(write_mbps);
    } else {
        disk["io_util_pct"] = Json::nullValue;
        disk["await_ms"] = Json::nullValue;
        disk["io_queue_depth"] = Json::nullValue;
        disk["read_mbps"] = Json::nullValue;
        disk["write_mbps"] = Json::nullValue;
    }
    disk["io_device"] = kernel_name;
    disk["io_state"] = diskIoLevelString(state);
    disk["is_main_storage"] = is_main_storage;
}

Json::Value DiskIoSnapshot::systemJson() const {
    Json::Value val;
    val["iowait_pct"] = has_iowait ? Json::Value(sanitize_for_json(iowait_pct)) : Json::Value(Json::nullValue);
    val["psi_io_some_pct"] = has_psi ? Json::Value(sanitize_for_json(psi_some_pct)) : Json::Value(Json::nullValue);
    val["psi_io_full_pct"] = has_psi ? Json::Value(sanitize_for_json(psi_full_pct)) : Json::Value(Json::nullValue);
    val["state"] = diskIoLevelString(system_state);
    return val;
}

const MountIoStats *DiskIoSnapshot::findMount(const std::string &mount_point) const {
    for (const auto &m : mounts) {
        if (m.mount_point == mount_point) {
            return &m;
        }
    }
    return nullptr;
}

////////////////////////////////////procfs parsing//////////////////////////////////////////

bool DiskIoMonitor::parseDiskStats(const std::string &text, std::unordered_map<std::string, DiskIoCounters> &out) {
    istringstream in(text);
    string line;
    bool any = false;
    while (getline(in, line)) {
        istringstream ls(line);
        unsigned major = 0, minor = 0;
        string name;
        uint64_t f[11] = {0};
        if (!(ls >> major >> minor >> name)) {
            continue;
        }
        int n = 0;
        while (n < 11 && (ls >> f[n])) {
            ++n;
        }
        if (n < 11) {
            continue;
        }
        DiskIoCounters c;
        c.reads = f[0];
        c.read_sectors = f[2];
        c.read_ms = f[3];
        c.writes = f[4];
        c.write_sectors = f[6];
        c.write_ms = f[7];
        c.io_ticks = f[9];
        c.weighted_ms = f[10];
        out[name] = c;
        any = true;
    }
    return any;
}

bool DiskIoMonitor::parseCpuStat(const std::string &text, uint64_t &iowait, uint64_t &total) {
    istringstream in(text);
    string line;
    while (getline(in, line)) {
        if (line.compare(0, 4, "cpu ") != 0) {
            continue;
        }
        istringstream ls(line.substr(4));
        // user nice system idle iowait irq softirq steal; guest/guest_nice are already in user/nice.
        uint64_t v[8] = {0};
        int n = 0;
        while (n < 8 && (ls >> v[n])) {
            ++n;
        }
        if (n < 5) {
            return false;
        }
        iowait = v[4];
        total = 0;
        for (int i = 0; i < n; ++i) {
            total += v[i];
        }
        return true;
    }
    return false;
}

bool DiskIoMonitor::parsePressure(const std::string &text, float &some_avg10, float &full_avg10) {
    istringstream in(text);
    string line;
    bool has_some = false;
    some_avg10 = 0.0f;
    full_avg10 = 0.0f;
    while (getline(in, line)) {
        auto pos = line.find("avg10=");
        if (pos == string::npos) {
            continue;
        }
        float value = strtof(line.c_str() + pos + 6, nullptr);
        if (line.compare(0, 4, "some") == 0) {
            some_avg10 = value;
            has_some = true;
        } else if (line.compare(0, 4, "full") == 0) {
            full_avg10 = value;
        }
    }
    return has_some;
}

static bool readWholeFile(const char *path, std::string &out) {
    // procfs reports size 0, so read until EOF instead of trusting stat().
    ifstream in(path);
    if (!in) {
        return false;
    }
    ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool DiskIoMonitor::readProcSample(DiskIoRawSample &out) {
#if defined(__linux__)
    out.at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
    string text;
    if (!readWholeFile("/proc/diskstats", text) || !parseDiskStats(text, out.disks)) {
        return false;
    }
    if (readWholeFile("/proc/stat", text)) {
        out.has_cpu = parseCpuStat(text, out.cpu_iowait, out.cpu_total);
    }
    if (readWholeFile("/proc/pressure/io", text)) {
        out.has_psi = parsePressure(text, out.psi_some_avg10, out.psi_full_avg10);
    }
    return true;
#else
    (void)out;
    return false;
#endif
}

std::string DiskIoMonitor::parentDiskName(const std::string &name) {
    size_t end = name.size();
    while (end > 0 && isdigit(static_cast<unsigned char>(name[end - 1]))) {
        --end;
    }
    if (end == name.size() || end == 0) {
        return "";
    }
    // nvme0n1p2 / mmcblk0p1: the partition suffix is "p<N>" after a digit.
    if (name[end - 1] == 'p' && end >= 2 && isdigit(static_cast<unsigned char>(name[end - 2]))) {
        return name.substr(0, end - 1);
    }
    // dm-0, md0, nvme0n1, loop0 end in digits but are whole devices.
    if (name[end - 1] == '-' || name.compare(0, 2, "md") == 0 || name.compare(0, 4, "nvme") == 0 ||
        name.compare(0, 6, "mmcblk") == 0 || name.compare(0, 4, "loop") == 0) {
        return "";
    }
    return name.substr(0, end);
}

std::string DiskIoMonitor::resolveKernelName(const std::string &device) {
    static const string kDev = "/dev/";
    if (device.compare(0, kDev.size(), kDev) != 0) {
        return "";
    }
#if defined(__linux__)
    char buf[PATH_MAX];
    if (realpath(device.c_str(), buf)) {
        string resolved = buf;
        auto pos = resolved.rfind('/');
        return pos == string::npos ? resolved : resolved.substr(pos + 1);
    }
    // In a container the /dev/mapper node may be missing while /sys/block still names it.
    static const string kMapper = "/dev/mapper/";
    if (device.compare(0, kMapper.size(), kMapper) == 0) {
        const string dm_name = device.substr(kMapper.size());
        DIR *dir = opendir("/sys/block");
        if (dir) {
            string found;
            struct dirent *ent;
            while ((ent = readdir(dir)) != nullptr) {
                string entry = ent->d_name;
                if (entry.compare(0, 3, "dm-") != 0) {
                    continue;
                }
                string name;
                if (readWholeFile(("/sys/block/" + entry + "/dm/name").c_str(), name) && trim(name) == dm_name) {
                    found = entry;
                    break;
                }
            }
            closedir(dir);
            return found;
        }
        return "";
    }
#endif
    return device.substr(device.rfind('/') + 1);
}

////////////////////////////////////DiskIoMonitor//////////////////////////////////////////

DiskIoMonitor::DiskIoMonitor(EventPoller::Ptr poller, const Providers &providers)
    : ResourceMonitor(ResourceType::DISK_IO, poller), _providers(providers) {
    if (!_providers.sample) {
        _providers.sample = &DiskIoMonitor::readProcSample;
    }
    if (!_providers.resolver) {
        _providers.resolver = &DiskIoMonitor::resolveKernelName;
    }
    if (!_providers.config) {
        _providers.config = []() { return DiskIoConfig(); };
    }
    if (!_providers.targets) {
        _providers.targets = []() { return std::vector<DiskIoTarget>(); };
    }
}

DiskIoMonitor::~DiskIoMonitor() {
    _timer.reset();
}

void DiskIoMonitor::start() {
    if (_timer) {
        return;
    }
    float interval = _providers.config().sample_interval_sec;
    if (interval <= 0) {
        interval = 5.0f;
    }
    sampleNow();
    weak_ptr<DiskIoMonitor> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(
        interval,
        [weak_self]() {
            auto self = weak_self.lock();
            if (!self) {
                return false;
            }
            self->sampleNow();
            return true;
        },
        _poller);
}

DiskIoSnapshot DiskIoMonitor::getSnapshot() {
    lock_guard<mutex> lck(_mtx);
    return _snapshot;
}

bool DiskIoMonitor::advance(AlertState &st, DiskIoLevel observed, int64_t now_ms, const DiskIoConfig &cfg) {
    return st.tracker.advance(static_cast<int>(observed), now_ms, cfg.sustain_samples,
                              static_cast<int64_t>(cfg.alert_repeat_sec) * 1000);
}

static void emitDiskIoAlert(const std::string &mount_point, const std::string &device, const std::string &metric,
                            float value, float threshold, DiskIoLevel level, bool is_main_storage) {
    const int level_value = static_cast<int>(level);
    if (level == DiskIoLevel::NORMAL) {
        InfoL << "Disk IO recovered on " << (mount_point.empty() ? "host" : mount_point) << " (" << metric << " "
              << format_float_2f(value) << ")";
    } else {
        WarnL << "Disk IO " << diskIoLevelString(level) << " on " << (mount_point.empty() ? "host" : mount_point)
              << ": " << metric << " " << format_float_2f(value) << " >= " << format_float_2f(threshold);
    }
    if (!NOTICE_EMIT(BroadcastDiskIoAlertArgs, mediakit::Broadcast::kBroadcastDiskIoAlert, mount_point, device, metric,
                     value, threshold, level_value, is_main_storage)) {
        TraceL << "No one listen disk io alert event";
    }
}

static inline uint64_t counterDelta(uint64_t cur, uint64_t prev, bool &ok) {
    if (cur < prev) {
        ok = false; // counter wrapped (32-bit kernel) or device was re-created
        return 0;
    }
    return cur - prev;
}

bool DiskIoMonitor::sampleNow() {
    const DiskIoConfig cfg = _providers.config();
    DiskIoRawSample cur;
    if (!_providers.sample(cur)) {
        WarnL << "Failed to sample disk IO counters";
        return false;
    }
    const auto targets = _providers.targets();

    const bool have_prev = _has_prev && cur.at_ms > _prev.at_ms;
    const double dt_ms = have_prev ? static_cast<double>(cur.at_ms - _prev.at_ms) : 0.0;

    DiskIoSnapshot snap;
    snap.sampled_at = static_cast<int64_t>(time(nullptr));
    snap.valid = true;

    std::map<std::string, AlertState> alerts;
    for (const auto &target : targets) {
        if (target.mount_point.empty() || snap.findMount(target.mount_point)) {
            continue;
        }
        MountIoStats m;
        m.mount_point = target.mount_point;
        m.device = target.device;
        m.is_main_storage = target.is_main_storage;
        m.kernel_name = _providers.resolver(target.device);
        if (!m.kernel_name.empty() && !cur.disks.count(m.kernel_name)) {
            // Old kernels may not list partitions; fall back to the whole disk.
            auto parent = parentDiskName(m.kernel_name);
            m.kernel_name = cur.disks.count(parent) ? parent : "";
        }

        AlertState st;
        auto old = _alerts.find(m.mount_point);
        if (old != _alerts.end()) {
            st = old->second;
        }

        auto cur_it = m.kernel_name.empty() ? cur.disks.end() : cur.disks.find(m.kernel_name);
        auto prev_it = have_prev && cur_it != cur.disks.end() ? _prev.disks.find(m.kernel_name) : _prev.disks.end();
        if (cur_it != cur.disks.end() && prev_it != _prev.disks.end()) {
            const auto &c = cur_it->second;
            const auto &p = prev_it->second;
            bool ok = true;
            const uint64_t d_reads = counterDelta(c.reads, p.reads, ok);
            const uint64_t d_writes = counterDelta(c.writes, p.writes, ok);
            const uint64_t d_rsec = counterDelta(c.read_sectors, p.read_sectors, ok);
            const uint64_t d_wsec = counterDelta(c.write_sectors, p.write_sectors, ok);
            const uint64_t d_rms = counterDelta(c.read_ms, p.read_ms, ok);
            const uint64_t d_wms = counterDelta(c.write_ms, p.write_ms, ok);
            const uint64_t d_ticks = counterDelta(c.io_ticks, p.io_ticks, ok);
            const uint64_t d_weighted = counterDelta(c.weighted_ms, p.weighted_ms, ok);
            if (ok) {
                const uint64_t ios = d_reads + d_writes;
                const double dt_sec = dt_ms / 1000.0;
                m.valid = true;
                m.util_pct = static_cast<float>(std::min(100.0, 100.0 * d_ticks / dt_ms));
                m.await_ms = ios ? static_cast<float>(static_cast<double>(d_rms + d_wms) / ios) : 0.0f;
                m.queue_depth = static_cast<float>(d_weighted / dt_ms);
                // diskstats sectors are always 512 bytes.
                m.read_mbps = static_cast<float>(d_rsec * 512.0 / dt_sec / (1024.0 * 1024.0));
                m.write_mbps = static_cast<float>(d_wsec * 512.0 / dt_sec / (1024.0 * 1024.0));
            }
        }

        if (m.valid) {
            // util alone misreads NVMe/RAID with parallel queues, so critical needs util and await.
            DiskIoLevel observed = DiskIoLevel::NORMAL;
            if (m.util_pct >= cfg.util_critical && m.await_ms >= cfg.await_critical_ms) {
                observed = DiskIoLevel::CRITICAL;
            } else if (m.util_pct >= cfg.util_warning || m.await_ms >= cfg.await_warning_ms) {
                observed = DiskIoLevel::WARNING;
            }
            if (advance(st, observed, cur.at_ms, cfg)) {
                // Latency is what stalls recording, so report await whenever it crossed its threshold.
                // Recovery reports the metric of the alert it closes, against its warning threshold.
                const DiskIoLevel state = static_cast<DiskIoLevel>(st.tracker.level);
                const bool critical = state == DiskIoLevel::CRITICAL;
                if (state != DiskIoLevel::NORMAL) {
                    st.last_metric = m.await_ms >= (critical ? cfg.await_critical_ms : cfg.await_warning_ms) ? "await" : "util";
                }
                const bool await_metric = st.last_metric == "await";
                const float threshold = await_metric ? (critical ? cfg.await_critical_ms : cfg.await_warning_ms)
                                                     : (critical ? cfg.util_critical : cfg.util_warning);
                emitDiskIoAlert(m.mount_point, m.device, st.last_metric, await_metric ? m.await_ms : m.util_pct,
                                threshold, state, m.is_main_storage);
            }
        }
        m.state = static_cast<DiskIoLevel>(st.tracker.level);
        alerts[m.mount_point] = st;
        snap.mounts.push_back(m);
    }

    // Host level: PSI when the kernel has it, otherwise the coarser /proc/stat iowait.
    AlertState host;
    auto old_host = _alerts.find(kHostAlertKey);
    if (old_host != _alerts.end()) {
        host = old_host->second;
    }
    bool host_observed = false;
    float host_value = 0.0f, host_warn = 0.0f, host_crit = 0.0f;
    const char *host_metric = "iowait";
    if (cur.has_psi) {
        snap.has_psi = true;
        snap.psi_some_pct = cur.psi_some_avg10;
        snap.psi_full_pct = cur.psi_full_avg10;
        host_observed = true;
        host_metric = "psi";
        host_value = cur.psi_full_avg10;
        host_warn = cfg.psi_full_warning;
        host_crit = cfg.psi_full_critical;
    }
    if (have_prev && cur.has_cpu && _prev.has_cpu && cur.cpu_total > _prev.cpu_total && cur.cpu_iowait >= _prev.cpu_iowait) {
        snap.has_iowait = true;
        snap.iowait_pct = static_cast<float>(100.0 * (cur.cpu_iowait - _prev.cpu_iowait) / (cur.cpu_total - _prev.cpu_total));
        if (!cur.has_psi) {
            host_observed = true;
            host_value = snap.iowait_pct;
            host_warn = cfg.iowait_warning;
            host_crit = cfg.iowait_critical;
        }
    }
    if (host_observed) {
        DiskIoLevel observed = DiskIoLevel::NORMAL;
        if (host_value >= host_crit) {
            observed = DiskIoLevel::CRITICAL;
        } else if (host_value >= host_warn) {
            observed = DiskIoLevel::WARNING;
        }
        if (advance(host, observed, cur.at_ms, cfg)) {
            const DiskIoLevel state = static_cast<DiskIoLevel>(host.tracker.level);
            emitDiskIoAlert("", "", host_metric, host_value,
                            state == DiskIoLevel::CRITICAL ? host_crit : host_warn, state, false);
        }
    }
    snap.system_state = static_cast<DiskIoLevel>(host.tracker.level);
    alerts[kHostAlertKey] = host;

    _alerts.swap(alerts);
    _prev = cur;
    _has_prev = true;
    {
        lock_guard<mutex> lck(_mtx);
        _snapshot = snap;
    }
    return true;
}

} // namespace managerkit
