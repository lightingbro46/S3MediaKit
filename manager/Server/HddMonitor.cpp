
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/statvfs.h>
#include <mntent.h>
#include <cstring>
#elif  defined(__APPLE__)
#include <sys/mount.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include "HddMonitor.h"
#include "Common/BlockingExecutors.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)

static bool is_read_only(const std::string& options) {
    std::istringstream ss(options);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token == "ro") return true;
        if (token == "rw") return false;  // ưu tiên xác định rõ "rw"
    }

    return false;  // không rõ → mặc định là ghi được
}

static bool is_virtual_filesystem(const std::string& fstype, const std::string& mount_point) {
    static const std::set<std::string> virtual_fs = {
        "tmpfs", "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "overlay",
        "squashfs", "rpc_pipefs", "securityfs", "pstore", "debugfs",
        "configfs", "fusectl", "mqueue", "hugetlbfs", "tracefs", "binfmt_misc", "drivers"
    };

    // loại hệ thống file ảo
    if (virtual_fs.count(fstype)) return true;

    // Network mounts are storage candidates even when mounted below a path
    // normally hidden from the system-resource dashboard. Keep read-only
    // mounts too so consumers can detect a writable mount becoming read-only.
    DiskPartition partition;
    partition.filesystem_type = fstype;
    if (partition.isNetworkFileSystem()) return false;

    // bỏ qua cả /snap, /run, /var/lib/docker, v.v.
    // mount point hệ thống
    static const std::vector<std::string> system_mounts = {
        "/boot", "/boot/efi", "/sys", "/proc", "/dev", "/run", "/snap",
        "/var", "/var/lib", "/var/lib/docker", "/var/lib/flatpak", "/tmp", "/etc"
    };
    for (const auto& prefix : system_mounts) {
        if (mount_point == prefix || mount_point.find(prefix + "/") == 0)
            return true;
    }

    return false;
}

#endif

#if defined(__linux__) || defined(__ANDROID__)

static bool get_disk_partitions(std::vector<DiskPartition> &result) {
    FILE* mtab = setmntent("/etc/mtab", "r");
    if (!mtab) return false;

    struct mntent* ent;
    while ((ent = getmntent(mtab)) != nullptr) {
        // Bỏ qua tmpfs, proc, sysfs, v.v.
        std::string fstype = ent->mnt_type;
        std::string mount_point = ent->mnt_dir;
        std::string opts = ent->mnt_opts;

        if (is_virtual_filesystem(fstype, mount_point)) continue;

        struct statvfs stat;
        if (statvfs(mount_point.c_str(), &stat) != 0)
            continue;

        uint64_t total = stat.f_blocks * stat.f_frsize;
        // f_bavail is the capacity the MediaServer process can actually use;
        // f_bfree may include filesystem-reserved blocks unavailable to it.
        uint64_t free = stat.f_bavail * stat.f_frsize;
        uint64_t used = total - free;

        DiskPartition part;
        part.device = ent->mnt_fsname;
        part.mount_point = mount_point;
        part.filesystem_type = fstype;
        part.read_only = is_read_only(opts);
        part.total_bytes = total;
        part.free_bytes = free;
        part.used_bytes = used;
        part.usage_pct = total == 0 ? 0.0f : 100.0f * static_cast<float>(used) / static_cast<float>(total);

        result.push_back(part);
    }

    endmntent(mtab);
    return true;
}

#elif defined(__APPLE__)

static bool get_disk_partitions(std::vector<DiskPartition> &result) {
    struct statfs* mounts;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count == 0) return false;

    for (int i = 0; i < count; ++i) {
        std::string fstype = mounts[i].f_fstypename;
        std::string mount_point = mounts[i].f_mntonname;

        if (is_virtual_filesystem(fstype, mount_point)) continue;

        uint64_t total = mounts[i].f_blocks * mounts[i].f_bsize;
        uint64_t free = mounts[i].f_bavail * mounts[i].f_bsize;
        uint64_t used = total - free;

        DiskPartition part;
        part.device = mounts[i].f_mntfromname;
        part.mount_point = mount_point;
        part.filesystem_type = fstype;
        part.read_only = (mounts[i].f_flags & MNT_RDONLY) != 0;
        part.total_bytes = total;
        part.free_bytes = free;
        part.used_bytes = used;
        part.usage_pct = total == 0 ? 0.0f : 100.0f * static_cast<float>(used) / static_cast<float>(total);

        result.push_back(part);
    }

    return true;
}

#elif defined(_WIN32)

static bool get_disk_partitions(std::vector<DiskPartition> &result) {

    DWORD size = GetLogicalDriveStringsA(0, nullptr);
    std::vector<char> buffer(size + 1);
    GetLogicalDriveStringsA(size, buffer.data());

    for (char* p = buffer.data(); *p; p += strlen(p) + 1) {
        std::string drive = p;

        UINT type = GetDriveTypeA(drive.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            continue;

        char fs_name[32];
        if (!GetVolumeInformationA(drive.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs_name, sizeof(fs_name)))
            continue;

        ULARGE_INTEGER free_bytes, total_bytes, total_free;
        if (!GetDiskFreeSpaceExA(drive.c_str(), &free_bytes, &total_bytes, &total_free))
            continue;

        // Bỏ qua ổ có dung lượng nhỏ hơn 100MB (likely system)
        if (total_bytes.QuadPart < 100ull * 1024 * 1024)
            continue;

        DiskPartition part;
        part.device = drive;
        part.mount_point = drive;
        part.filesystem_type = fs_name;
        part.total_bytes = total_bytes.QuadPart;
        part.free_bytes = total_free.QuadPart;
        part.used_bytes = total_bytes.QuadPart - total_free.QuadPart;
        part.usage_pct = total_bytes.QuadPart == 0 ? 0.0f
            : 100.0f * static_cast<float>(part.used_bytes) / static_cast<float>(part.total_bytes);

        result.push_back(part);
    }

    return true;
}
#else
static bool get_disk_partitions(std::vector<DiskPartition> &) {
    return false;
}
#endif

HddMonitor::HddMonitor(EventPoller::Ptr poller,
                       float refresh_interval_sec,
                       const PartitionProvider &provider)
    : ResourceMonitor(ResourceType::HDD, poller),
      _refresh_interval_sec(refresh_interval_sec > 0 ? refresh_interval_sec : 10.0f),
      _provider(provider ? provider : get_disk_partitions) {
}

HddMonitor::~HddMonitor() {
    _stopping = true;
    _refresh_timer.reset();
}

void HddMonitor::start() {
    if (_refresh_timer) {
        return;
    }
    refreshNow();
    weak_ptr<HddMonitor> weak_self = shared_from_this();
    _refresh_timer = std::make_shared<Timer>(
        _refresh_interval_sec,
        [weak_self]() {
            auto self = weak_self.lock();
            if (!self || self->_stopping) {
                return false;
            }
            self->dispatchRefresh();
            return true;
        },
        _poller);
}

void HddMonitor::dispatchRefresh() {
    if (_stopping || _refresh_running.exchange(true)) {
        return;
    }
    weak_ptr<HddMonitor> weak_self = shared_from_this();
    // statvfs on a hung NFS/NAS mount blocks in D state indefinitely, so never on a poller.
    // _refresh_running bounds a hung mount to one pool thread.
    BlockingExecutors::api()->async([weak_self]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        struct RefreshGuard {
            std::atomic<bool> &running;
            explicit RefreshGuard(std::atomic<bool> &flag) : running(flag) {}
            ~RefreshGuard() { running = false; }
        } guard(self->_refresh_running);
        if (!self->_stopping) {
            self->refreshNow();
        }
    });
}

bool HddMonitor::refreshNow() {
    std::lock_guard<std::mutex> refresh_lock(_refresh_mtx);
    std::vector<DiskPartition> partitions;
    if (!_provider(partitions)) {
        WarnL << "Failed to refresh disk mount snapshot";
        return false;
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    for (auto &partition : partitions) {
        while (partition.mount_point.size() > 1 && partition.mount_point.back() == '/') {
            partition.mount_point.pop_back();
        }
        partition.last_seen_at = now;
    }

    {
        lock_guard<mutex> lck(_mtx);
        DiskSnapshot next;
        next.partitions.swap(partitions);
        next.refreshed_at = now;
        next.generation = _snapshot.generation + 1;
        next.valid = true;
        _snapshot = next;
    }

    const auto snapshot = getSnapshot();
    for (const auto &partition : snapshot.partitions) {
        emitSystemAlert(partition.usage_pct);
    }
    return true;
}

DiskSnapshot HddMonitor::getSnapshot() {
    lock_guard<mutex> lck(_mtx);
    return _snapshot;
}

vector<DiskPartition> HddMonitor::getCurrentUsage() {
    return getSnapshot().partitions;
}

bool HddMonitor::findMountPoint(const DiskSnapshot &snapshot,
                                const std::string &path,
                                DiskPartition &out_partition) {
    if (!snapshot.valid || path.empty()) {
        return false;
    }
    bool found = false;
    for (const auto &partition : snapshot.partitions) {
        const std::string &mount_point = partition.mount_point;
        if (mount_point.empty()) {
            continue;
        }
        const bool belongs = mount_point == "/"
            ? path[0] == '/'
            : path == mount_point ||
              (path.size() > mount_point.size() &&
               path.compare(0, mount_point.size(), mount_point) == 0 &&
               path[mount_point.size()] == '/');
        if (belongs && (!found || mount_point.size() > out_partition.mount_point.size())) {
            out_partition = partition;
            found = true;
        }
    }
    return found;
}

} // namespace managerkit
