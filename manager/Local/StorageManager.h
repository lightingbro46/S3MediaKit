
#ifndef LOCAL_STORAGEMANAGER_H
#define LOCAL_STORAGEMANAGER_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "Util/TimeTicker.h"
#include "json/json.h"

namespace managerkit {

struct TimeBlockIdentity;
 
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

    size_t removeCameraTimeBlocks(const std::string &camera_id,
                                  const std::vector<TimeBlockIdentity> &blocks);

private:
    StorageManager(const toolkit::EventPoller::Ptr &poller = nullptr);

    // Queues one maintenance cycle on managerkit::BlockingExecutors::storage(). Everything the
    // cycle does is blocking (statvfs, walking the whole record tree, deleting files, rebuilding
    // timefiles, SQLite deletes), so none of it may run on an EventPoller.
    // Two cycles never run at the same time: on a large archive a cycle can outlast the 300s timer
    // interval, and queueing cycles up would only make the backlog grow. `full` false runs the
    // temporary-file cleanup only (what the constructor used to do at startup).
    void postMaintenanceCycle(bool full);

    // All three run on the storage pool, never on a poller.
    void cleanupTemporaryFiles();

    void enforceStoragePolicy();

    void removeExpiredUserSession();

    std::string getMainStorageMountPoint();

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
    // Taken before a cycle is queued (from _poller for the timer, from the caller of start() for
    // the first one), given back when the cycle's task is destroyed.
    std::atomic<bool> _cycle_running { false };
    // Only touched by cleanupTemporaryFiles(), i.e. only on the (single-threaded) storage pool.
    uint64_t _next_cleanup_time = 0;
};

} // namespace managerkit

#endif // LOCAL_STORAGEMANAGER_H
