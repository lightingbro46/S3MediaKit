
#ifndef LOCAL_STORAGEMANAGER_H
#define LOCAL_STORAGEMANAGER_H

#include <memory>
#include <string>
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "Util/TimeTicker.h"
#include "json/json.h"

namespace managerkit {
 
class StorageManager : public std::enable_shared_from_this<StorageManager> {
public:
    using Ptr = std::shared_ptr<StorageManager>;

    static StorageManager &Instance();
    ~StorageManager();

    void start();

    void stop();

    void getMainStorageUsage(size_t &used_bytes, size_t &total_bytes);

    void getBackUpStorageUsage(size_t &used_bytes, size_t &total_bytes);

    Json::Value makeSystemStorageJson();

    void rebuildTimeFile(const std::string &camera_id, uint64_t threshold);

private:
    StorageManager(const toolkit::EventPoller::Ptr &poller = nullptr);

    void cleanupTemporaryFiles();

    void enforceStoragePolicy();

    void removeExpiredUserSession();

    std::string getMainStorageMountPoint();

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
    toolkit::Ticker _ticker;
    uint64_t _next_cleanup_time = 0;
};

} // namespace managerkit

#endif // LOCAL_STORAGEMANAGER_H