
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

using namespace std;
using namespace toolkit;

namespace managerkit {

struct DiskStats {
    int64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t used_bytes = 0;
};

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

static bool is_virtual_filesystem(const std::string& fstype, const std::string& mount_point, const std::string& options = "") {
    static const std::set<std::string> virtual_fs = {
        "tmpfs", "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "overlay",
        "squashfs", "rpc_pipefs", "securityfs", "pstore", "debugfs",
        "configfs", "fusectl", "mqueue", "hugetlbfs", "tracefs", "binfmt_misc", "drivers"
    };

    // loại hệ thống file ảo
    if (virtual_fs.count(fstype)) return true;

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

    // tùy chọn readonly
    if (!options.empty() && is_read_only(options))
        return true;

    return false;
}

#endif

#if defined(__linux__) || defined(__ANDROID__)

static std::vector<DiskPartition> get_disk_partitions() {
    std::vector<DiskPartition> result;
    FILE* mtab = setmntent("/etc/mtab", "r");
    if (!mtab) return result;

    struct mntent* ent;
    while ((ent = getmntent(mtab)) != nullptr) {
        // Bỏ qua tmpfs, proc, sysfs, v.v.
        std::string fstype = ent->mnt_type;
        std::string mount_point = ent->mnt_dir;
        std::string opts = ent->mnt_opts;

        if (is_virtual_filesystem(fstype, mount_point, opts)) continue;

        struct statvfs stat;
        if (statvfs(mount_point.c_str(), &stat) != 0)
            continue;

        uint64_t total = stat.f_blocks * stat.f_frsize;
        uint64_t free = stat.f_bfree * stat.f_frsize;
        uint64_t used = total - free;

        DiskPartition part;
        part.device = ent->mnt_fsname;
        part.mount_point = mount_point;
        part.filesystem_type = fstype;
        part.total_bytes = total;
        part.free_bytes = free;
        part.used_bytes = used;

        result.push_back(part);
    }

    endmntent(mtab);
    return result;
}

#elif defined(__APPLE__)

static std::vector<DiskPartition> get_disk_partitions() {
    std::vector<DiskPartition> result;
    struct statfs* mounts;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count == 0) return result;

    for (int i = 0; i < count; ++i) {
        std::string fstype = mounts[i].f_fstypename;
        std::string mount_point = mounts[i].f_mntonname;

        if ((mounts[i].f_flags & MNT_RDONLY) != 0) continue;

        if (is_virtual_filesystem(fstype, mount_point)) continue;

        uint64_t total = mounts[i].f_blocks * mounts[i].f_bsize;
        uint64_t free = mounts[i].f_bfree * mounts[i].f_bsize;
        uint64_t used = total - free;

        DiskPartition part;
        part.device = mounts[i].f_mntfromname;
        part.mount_point = mount_point;
        part.filesystem_type = fstype;
        part.total_bytes = total;
        part.free_bytes = free;
        part.used_bytes = used;

        result.push_back(part);
    }

    return result;
}

#elif defined(_WIN32)

static std::vector<DiskPartition> get_disk_partitions() {
    std::vector<DiskPartition> result;

    DWORD size = GetLogicalDriveStringsA(0, nullptr);
    std::vector<char> buffer(size + 1);
    GetLogicalDriveStringsA(size, buffer.data());

    for (char* p = buffer.data(); *p; p += strlen(p) + 1) {
        std::string drive = p;

        UINT type = GetDriveTypeA(drive.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            continue;

        // Bỏ qua ổ có dung lượng nhỏ hơn 100MB (likely system)
        if (total_bytes.QuadPart < 100ull * 1024 * 1024)
            continue;

        char fs_name[32];
        if (!GetVolumeInformationA(drive.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs_name, sizeof(fs_name)))
            continue;

        ULARGE_INTEGER free_bytes, total_bytes, total_free;
        if (!GetDiskFreeSpaceExA(drive.c_str(), &free_bytes, &total_bytes, &total_free))
            continue;

        DiskPartition part;
        part.device = drive;
        part.mount_point = drive;
        part.filesystem_type = fs_name;
        part.total_bytes = total_bytes.QuadPart;
        part.free_bytes = total_free.QuadPart;
        part.used_bytes = total_bytes.QuadPart - total_free.QuadPart;

        result.push_back(part);
    }

    return result;
}
#endif

static DiskStats get_disk_usage(const std::string& path) {
    DiskStats usage;

#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) {
        return usage;  // lỗi → trả 0
    }

    usage.total_bytes = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
    usage.free_bytes  = static_cast<uint64_t>(stat.f_bfree) * stat.f_frsize;
    usage.used_bytes  = usage.total_bytes - usage.free_bytes;
    return usage;

#elif defined(_WIN32)
    ULARGE_INTEGER free_bytes_available, total_bytes, total_free_bytes;
    if (!GetDiskFreeSpaceExA(path.c_str(), &free_bytes_available, &total_bytes, &total_free_bytes)) {
        return usage;  // lỗi → trả 0
    }

    usage.total_bytes = total_bytes.QuadPart;
    usage.free_bytes  = total_free_bytes.QuadPart;
    usage.used_bytes  = usage.total_bytes - usage.free_bytes;
    return usage;

#else
    return usage;  // nền tảng không hỗ trợ
#endif
}

void HddCollector::collect() {
    DiskStats usage = get_disk_usage(_info.name);
    _info.total_bytes = usage.total_bytes;
    _info.free_bytes = usage.free_bytes;
    _info.used_bytes = usage.used_bytes;
    _info.usage_pct = _info.total_bytes == 0 ? 0.0 : (100.0 * (double)_info.used_bytes / _info.total_bytes);

    onCollect(_info);
}

void HddMonitor::start() {
    auto disks = get_disk_partitions();
    for (const auto &d : disks) {
        auto collector = std::make_shared<HddCollector>(d.mount_point, _poller);
        collector->setOnCollect([&](DiskUsage &info) {
            lock_guard<mutex> lck(_mtx);
            _map_result[info.name].total_bytes = info.total_bytes;
            _map_result[info.name].free_bytes = info.free_bytes;
            _map_result[info.name].used_bytes = info.used_bytes;
            _map_result[info.name].usage_pct = info.usage_pct;
            emitSystemAlert(info.usage_pct);
        });

        _map_collector[d.mount_point] = collector;
        _map_result[d.mount_point] = d;
    }
}

vector<DiskPartition> HddMonitor::getCurrentUsage() {
    lock_guard<mutex> lck(_mtx);
    vector<DiskPartition> ret;
    for (const auto &it : _map_result) {
        ret.push_back(it.second);
    }
    return ret;
}

} // namespace managerkit
