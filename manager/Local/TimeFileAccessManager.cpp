#include "TimeFileAccessManager.h"

#include <chrono>

#include "Poller/EventPoller.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

// While a swap is pending, readers wait so the file is never renamed under them.
// The finalize step is retried on a timer instead of blocking a worker thread, so a
// long-lived reader cannot pin a background thread for the whole read session.
static constexpr int kFinalizeRetryMs = 100;
static constexpr uint64_t kFinalizeWarnEveryAttempts = 100; // ~10s

struct TimeFileAccessState {
    mutex mtx;
    condition_variable cv;
    size_t reader_count = 0;
    bool pending_swap = false;
};

INSTANCE_IMP(TimeFileAccessManager)

shared_ptr<TimeFileAccessState> TimeFileAccessManager::getState(const string &file) {
    lock_guard<mutex> lock(_mutex);
    auto &state = _states[file];
    if (!state) {
        state = make_shared<TimeFileAccessState>();
    }
    return state;
}

TimeFileAccessManager::ReadGuard TimeFileAccessManager::acquireRead(const string &file) {
    auto state = getState(file);
    {
        unique_lock<mutex> lock(state->mtx);
        state->cv.wait(lock, [&]() { return !state->pending_swap; });
        ++state->reader_count;
    }
    return make_shared<toolkit::onceToken>(nullptr, [state, file]() {
        TimeFileAccessManager::Instance().releaseRead(state, file);
    });
}

void TimeFileAccessManager::releaseRead(const shared_ptr<TimeFileAccessState> &state, const string &file) {
    if (!state) {
        return;
    }
    bool maybe_idle = false;
    {
        lock_guard<mutex> lock(state->mtx);
        if (state->reader_count > 0) {
            --state->reader_count;
        } else {
            WarnL << "Release time file read guard with zero readers: " << file;
        }
        maybe_idle = state->reader_count == 0 && !state->pending_swap;
    }
    state->cv.notify_all();

    // Drop the per-file state once nobody is reading and no swap is pending to keep the
    // states map from growing unbounded over the lifetime of the process.
    if (maybe_idle) {
        lock_guard<mutex> lock(_mutex);
        auto it = _states.find(file);
        if (it != _states.end() && it->second == state) {
            lock_guard<mutex> state_lock(state->mtx);
            if (state->reader_count == 0 && !state->pending_swap) {
                _states.erase(it);
            }
        }
    }
}

void TimeFileAccessManager::beginPendingSwap(const string &file) {
    auto state = getState(file);
    {
        lock_guard<mutex> lock(state->mtx);
        state->pending_swap = true;
    }
    state->cv.notify_all();
}

void TimeFileAccessManager::finalizeSwapWhenReadableIdle(const string &file, const function<void()> &cb) {
    tryFinalizeSwap(file, cb, 0);
}

void TimeFileAccessManager::tryFinalizeSwap(const string &file, const function<void()> &cb, uint64_t attempts) {
    auto state = getState(file);

    bool idle = false;
    {
        unique_lock<mutex> lock(state->mtx);
        idle = state->cv.wait_for(lock, chrono::milliseconds(kFinalizeRetryMs),
                                  [&]() { return state->reader_count == 0; });
    }

    if (!idle) {
        if (attempts > 0 && attempts % kFinalizeWarnEveryAttempts == 0) {
            WarnL << "Waiting for readers to finish before swapping time file: " << file
                  << ", waited ~" << (attempts * kFinalizeRetryMs) / 1000 << "s";
        }
        // Re-schedule on a poller timer so we do not pin a worker thread while readers
        // are still active.
        auto next_attempt = attempts + 1;
        EventPollerPool::Instance().getPoller()->doDelayTask(kFinalizeRetryMs, [file, cb, next_attempt]() {
            TimeFileAccessManager::Instance().tryFinalizeSwap(file, cb, next_attempt);
            return 0;
        });
        return;
    }

    // No readers: perform the swap. Guarantee pending_swap is always cleared even if the
    // callback throws, otherwise future readers of this file would block forever.
    try {
        cb();
    } catch (std::exception &ex) {
        WarnL << "Time file swap callback failed for " << file << ": " << ex.what();
    }

    {
        lock_guard<mutex> lock(state->mtx);
        state->pending_swap = false;
    }
    state->cv.notify_all();

    // Clean up the state entry if it is now idle.
    {
        lock_guard<mutex> lock(_mutex);
        auto it = _states.find(file);
        if (it != _states.end() && it->second == state) {
            lock_guard<mutex> state_lock(state->mtx);
            if (state->reader_count == 0 && !state->pending_swap) {
                _states.erase(it);
            }
        }
    }
}

} // namespace managerkit
