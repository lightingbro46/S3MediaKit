#ifndef SERVER_STORAGEWRITEMONITOR_H
#define SERVER_STORAGEWRITEMONITOR_H

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "json/json.h"
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "Thread/TaskExecutor.h"

namespace managerkit {

// A directory the server must be able to write: the record path, the DB directory or a tier pool.
struct WriteCheckTarget {
    std::string path;
    std::string mount_point;     // empty when the mount is unknown
    std::string device;
    bool read_only = false;      // "ro" in the mount options
    bool is_main_storage = false;
};

enum class StorageWriteState : int {
    WRITABLE = 0,
    READ_ONLY = 1,      // mounted ro, or a write failed with EROFS
    WRITE_FAILED = 2,   // any other write error (EIO, EACCES, ENOSPC, ...)
};

const char *storageWriteStateString(StorageWriteState state);

struct StorageWriteStatus {
    std::string path;
    std::string mount_point;
    std::string device;
    bool is_main_storage = false;
    bool checked = false;
    StorageWriteState state = StorageWriteState::WRITABLE;
    int error = 0;               // errno of the failed write, 0 otherwise
    int64_t checked_at = 0;      // unix seconds

    Json::Value toJson() const;
};

/**
 * Raises kBroadcastDiskWriteAlert when a storage directory stops accepting writes (read-only
 * remount, IO error, lost permission) and again when it recovers.
 *
 * The timer runs on the poller only to collect targets; each write probe (create + fsync + unlink)
 * runs on the blocking executor. A path whose previous probe is still running is skipped, so a hung
 * NFS mount holds at most one thread.
 */
class StorageWriteMonitor : public std::enable_shared_from_this<StorageWriteMonitor> {
public:
    using Ptr = std::shared_ptr<StorageWriteMonitor>;
    using TargetProvider = std::function<std::vector<WriteCheckTarget>()>;
    // Returns 0 when `dir` accepts a write, otherwise the errno of the failure.
    using Probe = std::function<int(const std::string &dir)>;
    using RepeatProvider = std::function<int()>;

    StorageWriteMonitor(toolkit::EventPoller::Ptr poller,
                        float interval_sec,
                        const TargetProvider &targets,
                        const Probe &probe = Probe(),
                        toolkit::TaskExecutor::Ptr executor = nullptr,
                        const RepeatProvider &repeat_sec = RepeatProvider());
    ~StorageWriteMonitor();

    void start();

    // Probes one target on the calling thread (blocking) and emits on a state change.
    void checkTarget(const WriteCheckTarget &target);

    std::vector<StorageWriteStatus> getStatus();

    // Writes, fsyncs and removes a small file in `dir` (or its nearest existing parent).
    static int probeWritable(const std::string &dir);

private:
    void dispatchChecks();

private:
    struct PathState {
        StorageWriteStatus status;
        bool in_flight = false;
        int64_t last_emit = 0;
    };

    toolkit::EventPoller::Ptr _poller;
    float _interval_sec;
    TargetProvider _targets;
    Probe _probe;
    toolkit::TaskExecutor::Ptr _executor;
    RepeatProvider _repeat_sec;
    toolkit::Timer::Ptr _timer;
    std::mutex _mtx;
    std::map<std::string, PathState> _states; // path -> state
};

} // namespace managerkit

#endif // SERVER_STORAGEWRITEMONITOR_H
