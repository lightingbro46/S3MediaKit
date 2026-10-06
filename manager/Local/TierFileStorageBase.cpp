#include "TierFileStorageBase.h"

#ifdef ENABLE_TIER_STORAGE

#include "Util/File.h"
#include "Util/logger.h"
#include "Util/TimeTicker.h"
#include "Common/config.h"
#include "StorageTier.h"
#include "Server/GlobalMonitor.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

bool TierFileStorageBase::validatePoolPath(const std::string &base_path,
                                           std::string &out_message, int &out_latency_ms) const {
    Ticker ticker;
    struct stat st{};
    if (base_path.empty()) {
        out_message = "Path is empty";
        return false;
    }

    if (::stat(base_path.c_str(), &st) != 0) {
        if (errno != ENOENT) {
            out_message = std::string("Cannot stat path: ") + base_path + ": " + strerror(errno);
            return false;
        }
        std::string mkdir_path = base_path;
        if (mkdir_path.back() != '/')
            mkdir_path += '/';
        if (!File::create_path(mkdir_path, 0755)) {
            out_message = "Cannot create path: " + base_path;
            return false;
        }
        if (::stat(base_path.c_str(), &st) != 0) {
            out_message = std::string("Cannot create path: ") + base_path + ": " + strerror(errno);
            return false;
        }
    }
    if (!S_ISDIR(st.st_mode)) {
        out_message = "Path is not a directory: " + base_path;
        return false;
    }

    GET_CONFIG(string, media_server_id, General::kMediaServerId);
    std::string test_file = base_path;
    if (test_file.back() != '/')
        test_file += '/';
    test_file += media_server_id + ".tmp";

    int fd = ::open(test_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        out_message = std::string("Cannot write to path: ") + base_path + ": " + strerror(errno);
        return false;
    }
    ::close(fd);
    ::unlink(test_file.c_str());
    out_message = "Connection successful";
    out_latency_ms = ticker.elapsedTime();
    return true;
}

std::string TierFileStorageBase::normalizeBasePath(const std::string &base_path) {
    std::string path = base_path;
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

std::string TierFileStorageBase::normalizeKey(const std::string &key) {
    std::string ret = key;
    while (!ret.empty() && ret.front() == '/')
        ret.erase(ret.begin());
    return ret;
}

std::string TierFileStorageBase::parentDir(const std::string &path) {
    auto pos = path.rfind('/');
    if (pos == std::string::npos)
        return "";
    return path.substr(0, pos);
}

bool TierFileStorageBase::ensureDirectory(const std::string &dir) const {
    if (dir.empty())
        return true;

    std::string mkdir_path = dir;
    if (mkdir_path.back() != '/')
        mkdir_path += '/';
    if (!File::create_path(mkdir_path, 0755)) {
        WarnL << storageName() << "::ensureDirectory: create_path failed " << dir;
        return false;
    }

    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
        WarnL << storageName() << "::ensureDirectory: stat failed " << dir
              << ": " << strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        WarnL << storageName() << "::ensureDirectory: path is not a directory " << dir;
        return false;
    }
    return true;
}

bool TierFileStorageBase::registerPool(const std::string &pool_id,
                                       const std::string &type,
                                       const std::string &base_path,
                                       const std::string &expected_mount_path) {
    if (pool_id.empty()) {
        WarnL << storageName() << "::registerPool: pool_id is empty";
        return false;
    }

    std::string normalized = normalizeBasePath(base_path);
    std::string message;
    int latency_ms = 0;
    if (!validatePoolPath(normalized, message, latency_ms)) {
        WarnL << storageName() << "::registerPool: " << message << " pool=" << pool_id;
        return false;
    }

    PoolEntry entry;
    entry.pool_id = pool_id;
    entry.type = type;
    entry.base_path = normalized;
    if (type == poolTypeToString(StoragePoolType::NAS) && !expected_mount_path.empty()) {
        DiskPartition partition;
        if (!GlobalMonitor::Instance().findMountPointInfo(normalized, partition, true)) {
            WarnL << storageName() << "::registerPool: mount snapshot is unavailable pool=" << pool_id;
            return false;
        }
        const std::string expected = normalizeBasePath(expected_mount_path);
        if (partition.mount_point != expected && partition.mount_point != normalized) {
            WarnL << storageName() << "::registerPool: configured NAS path is not mounted pool="
                  << pool_id << " expected=" << expected << " actual=" << partition.mount_point;
            return false;
        }
        if (partition.read_only) {
            WarnL << storageName() << "::registerPool: NAS mount is read-only pool=" << pool_id;
            return false;
        }
        entry.mount_point = partition.mount_point;
        entry.mount_device = partition.device;
        entry.mount_filesystem_type = partition.filesystem_type;
    }

    {
        std::lock_guard<std::mutex> lk(_mtx);
        _pool_map[pool_id] = entry;
    }
    // Watch the pool's disk for IO congestion; NFS/CIFS mounts are skipped by the monitor.
    GlobalMonitor::Instance().setIoWatchPath("tier:" + pool_id, normalized);
    InfoL << storageName() << ": registered pool " << pool_id << " path=" << normalized;
    return true;
}

void TierFileStorageBase::unregisterPool(const std::string &pool_id) {
    {
        std::lock_guard<std::mutex> lk(_mtx);
        _pool_map.erase(pool_id);
    }
    GlobalMonitor::Instance().setIoWatchPath("tier:" + pool_id, "");
}

bool TierFileStorageBase::isRegistered(const std::string &pool_id) const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _pool_map.count(pool_id) > 0;
}

bool TierFileStorageBase::getPoolEntry(const std::string &pool_id,
                                       PoolEntry &entry) const {
    std::lock_guard<std::mutex> lk(_mtx);
    auto it = _pool_map.find(pool_id);
    if (it == _pool_map.end())
        return false;
    entry = it->second;
    return true;
}

bool TierFileStorageBase::testConnection(const std::string &pool_id,
                                         std::string &out_message, int &out_latency_ms) const {
    PoolEntry entry;
    if (!getPoolEntry(pool_id, entry)) {
        out_message = "Pool not registered: " + pool_id;
        return false;
    }
    return validatePoolPath(entry.base_path, out_message, out_latency_ms);
}

bool TierFileStorageBase::testConnectionParams(const std::string &base_path,
                                               std::string &out_message, int &out_latency_ms) const {
    return validatePoolPath(normalizeBasePath(base_path), out_message, out_latency_ms);
}

std::string TierFileStorageBase::resolvePath(const std::string &pool_id,
                                             const std::string &storage_key) const {
    PoolEntry entry;
    if (!getPoolEntry(pool_id, entry))
        return "";

    std::string key = normalizeKey(storage_key);
    if (key.empty())
        return entry.base_path;
    return entry.base_path + "/" + key;
}

bool TierFileStorageBase::copyFile(const std::string &src,
                                   const std::string &dst) const {
    int src_fd = ::open(src.c_str(), O_RDONLY);
    if (src_fd < 0) {
        WarnL << storageName() << "::copyFile: cannot open source " << src
              << ": " << strerror(errno);
        return false;
    }

    struct stat st{};
    if (::fstat(src_fd, &st) != 0) {
        WarnL << storageName() << "::copyFile: stat failed " << src
              << ": " << strerror(errno);
        ::close(src_fd);
        return false;
    }

    auto dir = parentDir(dst);
    if (!ensureDirectory(dir)) {
        ::close(src_fd);
        return false;
    }

    std::string tmp = dst + ".tmp";
    int dst_fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        WarnL << storageName() << "::copyFile: cannot create destination " << tmp
              << ": " << strerror(errno);
        ::close(src_fd);
        return false;
    }

    off_t offset = 0;
    off_t total = 0;
    while (total < st.st_size) {
        ssize_t sent = ::sendfile(dst_fd, src_fd, &offset,
                                  static_cast<size_t>(st.st_size - total));
        if (sent <= 0) {
            if (errno == EINTR)
                continue;
            WarnL << storageName() << "::copyFile: sendfile failed " << src
                  << " -> " << tmp << ": " << strerror(errno);
            ::close(src_fd);
            ::close(dst_fd);
            ::unlink(tmp.c_str());
            return false;
        }
        total += sent;
    }

    ::close(src_fd);
    // Network filesystems often report write errors only at fsync/close. The
    // caller deletes the source after a successful copy, so both must succeed.
    if (::fsync(dst_fd) != 0) {
        WarnL << storageName() << "::copyFile: fsync failed " << tmp
              << ": " << strerror(errno);
        ::close(dst_fd);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::close(dst_fd) != 0) {
        WarnL << storageName() << "::copyFile: close failed " << tmp
              << ": " << strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }

    if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        WarnL << storageName() << "::copyFile: rename failed " << tmp
              << " -> " << dst << ": " << strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }

    struct stat dst_st{};
    if (::stat(dst.c_str(), &dst_st) != 0 || dst_st.st_size != st.st_size) {
        WarnL << storageName() << "::copyFile: size mismatch after copy " << src
              << " -> " << dst << " expected=" << st.st_size
              << " actual=" << dst_st.st_size;
        ::unlink(dst.c_str());
        return false;
    }

    // Persist the rename itself; failure here is logged but not fatal
    // because the data file was already flushed.
    int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        if (::fsync(dir_fd) != 0) {
            WarnL << storageName() << "::copyFile: directory fsync failed " << dir
                  << ": " << strerror(errno);
        }
        ::close(dir_fd);
    }
    return true;
}

bool TierFileStorageBase::uploadSegment(const std::string &pool_id,
                                        const std::string &local_path,
                                        const std::string &storage_key) {
    auto dst = resolvePath(pool_id, storage_key);
    if (dst.empty()) {
        WarnL << storageName() << "::uploadSegment: pool not found " << pool_id;
        return false;
    }
    return copyFile(local_path, dst);
}

bool TierFileStorageBase::uploadSegmentAtomic(const std::string &pool_id,
                                              const std::string &local_path,
                                              const std::string &storage_key,
                                              const std::string &job_id) {
    const std::string destination = resolvePath(pool_id, storage_key);
    if (destination.empty() || job_id.empty())
        return false;
    const std::string temporary = destination + ".part-" + job_id;
    if (!copyFile(local_path, temporary))
        return false;
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
        WarnL << storageName() << "::uploadSegmentAtomic: rename failed "
              << temporary << " -> " << destination << ": " << strerror(errno);
        ::unlink(temporary.c_str());
        return false;
    }
    const std::string directory = parentDir(destination);
    const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        if (::fsync(dir_fd) != 0)
            WarnL << storageName() << "::uploadSegmentAtomic: directory fsync failed "
                  << directory << ": " << strerror(errno);
        ::close(dir_fd);
    }
    return true;
}

bool TierFileStorageBase::downloadSegment(const std::string &pool_id,
                                          const std::string &storage_key,
                                          const std::string &local_path) {
    auto src = resolvePath(pool_id, storage_key);
    if (src.empty()) {
        WarnL << storageName() << "::downloadSegment: pool not found " << pool_id;
        return false;
    }
    return copyFile(src, local_path);
}

bool TierFileStorageBase::deleteSegment(const std::string &pool_id,
                                        const std::string &storage_key) {
    auto path = resolvePath(pool_id, storage_key);
    if (path.empty()) {
        WarnL << storageName() << "::deleteSegment: pool not found " << pool_id;
        return false;
    }
    if (::unlink(path.c_str()) == 0)
        return true;
    if (errno == ENOENT)
        return true;
    WarnL << storageName() << "::deleteSegment: unlink failed " << path
          << ": " << strerror(errno);
    return false;
}

bool TierFileStorageBase::deleteSegmentsBatch(const std::string &pool_id,
                                              const std::vector<std::string> &storage_keys) {
    bool ok = true;
    for (const auto &key : storage_keys) {
        if (!deleteSegment(pool_id, key))
            ok = false;
    }
    return ok;
}

bool TierFileStorageBase::segmentExists(const std::string &pool_id,
                                        const std::string &storage_key) const {
    auto path = resolvePath(pool_id, storage_key);
    if (path.empty())
        return false;
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

TierPoolStats TierFileStorageBase::getPoolStats(const std::string &pool_id) const {
    TierPoolStats stats;
    PoolEntry entry;
    if (!getPoolEntry(pool_id, entry))
        return stats;

    if (entry.type == poolTypeToString(StoragePoolType::NAS) && !entry.mount_point.empty()) {
        DiskPartition partition;
        if (!GlobalMonitor::Instance().findMountPointInfo(entry.base_path, partition, true) ||
            partition.mount_point != entry.mount_point ||
            partition.device != entry.mount_device ||
            partition.filesystem_type != entry.mount_filesystem_type ||
            partition.read_only) {
            return stats;
        }
    }

    struct statvfs vfs{};
    if (::statvfs(entry.base_path.c_str(), &vfs) != 0)
        return stats;

    stats.is_online = true;
    stats.capacity_available = true;
    stats.capacity_status = "AVAILABLE";
    stats.total_bytes = static_cast<uint64_t>(vfs.f_blocks) * static_cast<uint64_t>(vfs.f_frsize);
    stats.free_bytes = static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
    stats.used_bytes = stats.total_bytes > stats.free_bytes
        ? stats.total_bytes - stats.free_bytes : 0;
    return stats;
}

std::string TierFileStorageBase::makeStorageKey(const std::string &camera_id,
                                                const std::string &stream_id,
                                                const std::string &segment_path) {
    return camera_id + "/" + stream_id + "/" + segment_path + ".mp4";
}

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
