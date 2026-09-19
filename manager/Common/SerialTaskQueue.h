#ifndef COMMON_SERIALTASKQUEUE_H
#define COMMON_SERIALTASKQUEUE_H

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "Thread/TaskExecutor.h"

namespace managerkit {

/**
 * Runs tasks one at a time, in submission order, on a shared executor (a "strand").
 *
 * Used to serialize blocking calls to one device (ONVIF SOAP, vendor SDK) on a shared pool:
 * two commands for the same camera never run concurrently or out of order, while different
 * devices still run in parallel. At most one task of a queue is handed to the executor at a
 * time, so a slow device occupies at most one pool thread.
 *
 * Pending tasks keep the queue alive until they have run; a task that throws is logged and
 * the queue continues with the next one.
 */
class SerialTaskQueue : public std::enable_shared_from_this<SerialTaskQueue> {
public:
    using Ptr = std::shared_ptr<SerialTaskQueue>;
    using Task = std::function<void()>;

    SerialTaskQueue(toolkit::TaskExecutor::Ptr executor, std::string name);

    // Thread-safe.
    void post(Task task);

    // Tasks waiting to run, excluding the one currently running.
    size_t pending() const;

private:
    void runNext();

private:
    toolkit::TaskExecutor::Ptr _executor;
    std::string _name;
    mutable std::mutex _mtx;
    std::deque<Task> _tasks;
    bool _running = false;
};

} // namespace managerkit

#endif // COMMON_SERIALTASKQUEUE_H
