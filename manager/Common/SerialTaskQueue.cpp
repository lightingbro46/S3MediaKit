#include "Common/SerialTaskQueue.h"

#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

SerialTaskQueue::SerialTaskQueue(TaskExecutor::Ptr executor, string name)
    : _executor(std::move(executor)), _name(std::move(name)) {}

void SerialTaskQueue::post(Task task) {
    {
        lock_guard<mutex> lck(_mtx);
        _tasks.push_back(std::move(task));
        if (_running) {
            // The running task schedules the next one when it finishes.
            return;
        }
        _running = true;
    }
    auto self = shared_from_this();
    _executor->async([self]() { self->runNext(); }, false);
}

size_t SerialTaskQueue::pending() const {
    lock_guard<mutex> lck(_mtx);
    return _tasks.size();
}

void SerialTaskQueue::runNext() {
    Task task;
    {
        lock_guard<mutex> lck(_mtx);
        task = std::move(_tasks.front());
        _tasks.pop_front();
    }
    try {
        task();
    } catch (std::exception &ex) {
        WarnL << "Task of queue " << _name << " threw: " << ex.what();
    } catch (...) {
        WarnL << "Task of queue " << _name << " threw an unknown exception";
    }
    {
        lock_guard<mutex> lck(_mtx);
        if (_tasks.empty()) {
            _running = false;
            return;
        }
    }
    // Re-post instead of looping so tasks of other queues sharing the executor get a turn.
    auto self = shared_from_this();
    _executor->async([self]() { self->runNext(); }, false);
}

} // namespace managerkit
