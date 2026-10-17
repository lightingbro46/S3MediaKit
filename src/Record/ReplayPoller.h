#ifndef S3MEDIAKIT_REPLAYPOLLER_H
#define S3MEDIAKIT_REPLAYPOLLER_H

#include <mutex>
#include "Common/config.h"
#include "Poller/EventPoller.h"
#include "Thread/ThreadPool.h"
#include "Thread/WorkThreadPool.h"

namespace mediakit {

// Replay timers never share the work pollers used for maintenance. Explicitly supplied pollers
// are still honored by the readers. Config is read once; zero retains the legacy pool for A/B runs.
class ReplayPollerPool : public toolkit::TaskExecutorGetterImp {
public:
    static ReplayPollerPool &Instance() {
        static ReplayPollerPool pool;
        return pool;
    }
    toolkit::EventPoller::Ptr getPoller() {
        if (_threads.empty()) return toolkit::WorkThreadPool::Instance().getPoller();
        std::lock_guard<std::mutex> lock(_mutex);
        return std::static_pointer_cast<toolkit::EventPoller>(getExecutor());
    }
private:
    ReplayPollerPool() {
        GET_CONFIG(uint32_t, threads, Record::kReplayPollerThreads);
        if (threads) {
            // Same priority as the work pool it replaces: ingest pollers (HIGHEST, SCHED_FIFO when
            // permitted) must keep preempting replay, and A/B with 0 changes only the pool.
            // Preserve process affinity; do not pin replay threads outside the test's cpuset.
            addPoller("replay poller", threads, toolkit::ThreadPool::PRIORITY_LOWEST, true, false);
        }
    }
    std::mutex _mutex;
};

} // namespace mediakit
#endif
