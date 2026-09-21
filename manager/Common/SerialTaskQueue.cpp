#include "Common/SerialTaskQueue.h"

#include <algorithm>

#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

SerialTaskQueue::SerialTaskQueue(TaskExecutor::Ptr executor, string name)
    : _executor(std::move(executor)), _name(std::move(name)) {}

void SerialTaskQueue::post(Task task) {
    postTask(std::move(task), false);
}

void SerialTaskQueue::postFront(Task task) {
    postTask(std::move(task), true);
}

void SerialTaskQueue::postTask(Task task, bool priority) {
    {
        lock_guard<mutex> lck(_mtx);
        if (priority) {
            // Ahead of every background task, but BEHIND the priority tasks already waiting: two
            // commands a user sent in a row (a PTZ move then a preset) must still reach the camera
            // in that order, or the camera ends up wherever the older one pointed.
            auto pos = std::find_if(_tasks.begin(), _tasks.end(),
                                    [](const QueuedTask &queued) { return !queued.priority; });
            _tasks.insert(pos, QueuedTask(std::move(task), true));
        } else {
            _tasks.emplace_back(std::move(task), false);
        }
        if (_running) {
            // The running task schedules the next one when it finishes; it picks the turn up with
            // the priority of whatever is at the front by then.
            return;
        }
        _running = true;
    }
    schedule(priority);
}

void SerialTaskQueue::schedule(bool priority) {
    auto self = shared_from_this();
    TaskIn run = [self]() { self->runNext(); };
    if (priority) {
        _executor->async_first(std::move(run), false);
    } else {
        _executor->async(std::move(run), false);
    }
}

size_t SerialTaskQueue::pending() const {
    lock_guard<mutex> lck(_mtx);
    return _tasks.size();
}

void SerialTaskQueue::runNext() {
    QueuedTask task;
    {
        lock_guard<mutex> lck(_mtx);
        task = std::move(_tasks.front());
        _tasks.pop_front();
    }
    try {
        task.task();
    } catch (std::exception &ex) {
        WarnL << "Task of queue " << _name << " threw: " << ex.what();
    } catch (...) {
        WarnL << "Task of queue " << _name << " threw an unknown exception";
    }
    bool next_priority;
    {
        lock_guard<mutex> lck(_mtx);
        if (_tasks.empty()) {
            _running = false;
            return;
        }
        next_priority = _tasks.front().priority;
    }
    // Re-post instead of looping so tasks of other queues sharing the executor get a turn.
    schedule(next_priority);
}

} // namespace managerkit
