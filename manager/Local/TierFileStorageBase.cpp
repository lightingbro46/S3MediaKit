#include "TierFileStorageBase.h"

#include "Util/File.h"
#include "Util/logger.h"
#include "Util/TimeTicker.h"
#include "Common/config.h"

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

bool TierFileStorageBase::registerPool(const std::string &pool_id,
                                       const std::string &type,
                                       const std::string &base_path) {
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

    {
        std::lock_guard<std::mutex> lk(_mtx);
        _pool_map[pool_id] = entry;
    }
    InfoL << storageName() << ": registered pool " << pool_id << " path=" << normalized;
    return true;
}

void TierFileStorageBase::unregisterPool(const std::string &pool_id) {
    std::lock_guard<std::mutex> lk(_mtx);
    _pool_map.erase(pool_id);
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
    if (!dir.empty())
        File::create_path(dir, 0755);

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

    ::fsync(dst_fd);
    ::close(src_fd);
    ::close(dst_fd);

    if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        WarnL << storageName() << "::copyFile: rename failed " << tmp
              << " -> " << dst << ": " << strerror(errno);
        ::unlink(tmp.c_str());
        return false;
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

    struct statvfs vfs{};
    if (::statvfs(entry.base_path.c_str(), &vfs) != 0)
        return stats;

    stats.is_online = true;
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
