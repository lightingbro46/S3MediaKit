#ifndef LOCAL_TIER_FILE_STORAGE_BASE_H
#define LOCAL_TIER_FILE_STORAGE_BASE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "TierObjectStorage.h"

namespace managerkit {

class TierFileStorageBase {
public:
    TierFileStorageBase() = default;
    virtual ~TierFileStorageBase() = default;

    bool registerPool(const std::string &pool_id,
                      const std::string &type,
                      const std::string &base_path);

    void unregisterPool(const std::string &pool_id);

    bool isRegistered(const std::string &pool_id) const;

    bool testConnection(const std::string &pool_id, std::string &out_message, int &out_latency_ms) const;

    bool testConnectionParams(const std::string &base_path,
                              std::string &out_message, int &out_latency_ms) const;

    bool uploadSegment(const std::string &pool_id,
                       const std::string &local_path,
                       const std::string &storage_key);

    bool downloadSegment(const std::string &pool_id,
                         const std::string &storage_key,
                         const std::string &local_path);

    bool deleteSegment(const std::string &pool_id,
                       const std::string &storage_key);

    bool deleteSegmentsBatch(const std::string &pool_id,
                             const std::vector<std::string> &storage_keys);

    bool segmentExists(const std::string &pool_id,
                       const std::string &storage_key) const;

    TierPoolStats getPoolStats(const std::string &pool_id) const;

    std::string resolvePath(const std::string &pool_id,
                            const std::string &storage_key) const;

    static std::string makeStorageKey(const std::string &camera_id,
                                      const std::string &stream_id,
                                      const std::string &segment_path);

    static std::string normalizeBasePath(const std::string &base_path);

protected:
    struct PoolEntry {
        std::string pool_id;
        std::string type;
        std::string base_path;
    };

    virtual const char *storageName() const = 0;

    virtual bool validatePoolPath(const std::string &base_path,
                                  std::string &out_message, int &out_latency_ms) const;

    bool getPoolEntry(const std::string &pool_id, PoolEntry &entry) const;

    bool copyFile(const std::string &src,
                  const std::string &dst) const;

    static std::string normalizeKey(const std::string &key);
    static std::string parentDir(const std::string &path);

private:
    mutable std::mutex _mtx;
    std::unordered_map<std::string, PoolEntry> _pool_map;
};

} // namespace managerkit

#endif // LOCAL_TIER_FILE_STORAGE_BASE_H
