#include "TimeFileAccessManager.h"
#include "Util/logger.h"

using namespace std;

namespace managerkit {

struct TimeFileAccessState {
    mutex mtx;
    condition_variable cv;
    size_t reader_count = 0;
    bool pending_swap = false;
};

TimeFileAccessManager &TimeFileAccessManager::Instance() {
    static TimeFileAccessManager instance;
    return instance;
}

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
    {
        lock_guard<mutex> lock(state->mtx);
        if (state->reader_count > 0) {
            --state->reader_count;
        } else {
            WarnL << "Release time file read guard with zero readers: " << file;
        }
    }
    state->cv.notify_all();
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
    auto state = getState(file);
    {
        unique_lock<mutex> lock(state->mtx);
        state->cv.wait(lock, [&]() { return state->reader_count == 0; });
    }

    cb();

    {
        lock_guard<mutex> lock(state->mtx);
        state->pending_swap = false;
    }
    state->cv.notify_all();
}

} // namespace managerkit
