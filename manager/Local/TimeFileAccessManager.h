#ifndef S3MEDIAKIT_TIMEFILEACCESSMANAGER_H_
#define S3MEDIAKIT_TIMEFILEACCESSMANAGER_H_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "Util/onceToken.h"
#include "Thread/TaskExecutor.h"

namespace managerkit {

class TimeFileAccessManager {
public:
    using ReadGuard = std::shared_ptr<toolkit::onceToken>;

    static TimeFileAccessManager &Instance();

    ReadGuard acquireRead(const std::string &file);
    void beginPendingSwap(const std::string &file);
    // Runs `cb` on the dedicated swap thread once the file has no readers.
    // Callers must not wrap this in a shared worker pool: a worker blocked in
    // acquireRead() on this file would never run the swap that unblocks it.
    void finalizeSwapWhenReadableIdle(const std::string &file, const std::function<void()> &cb);

private:
    TimeFileAccessManager() = default;

    std::shared_ptr<struct TimeFileAccessState> getState(const std::string &file);
    void releaseRead(const std::shared_ptr<struct TimeFileAccessState> &state, const std::string &file);
    void tryFinalizeSwap(const std::string &file, const std::function<void()> &cb, uint64_t attempts);

private:
    toolkit::TaskExecutor::Ptr swapExecutor();

    std::mutex _mutex;
    std::unordered_map<std::string, std::shared_ptr<struct TimeFileAccessState>> _states;
    std::mutex _executor_mutex;
    toolkit::TaskExecutor::Ptr _swap_executor;
};

} // namespace managerkit

#endif // S3MEDIAKIT_TIMEFILEACCESSMANAGER_H_
