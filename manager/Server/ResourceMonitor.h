#ifndef SERVER_RESOUCEMONITOR_H
#define SERVER_RESOUCEMONITOR_H

#include <memory>
#include <mutex>
#include "Poller/Timer.h"

namespace managerkit {

template <typename T>
class MetricCollector {
public:
    using Ptr = std::shared_ptr<MetricCollector>;

    MetricCollector(toolkit::EventPoller::Ptr poller, double interval_sec = 10.0f) : _poller(poller) {
        _timer = std::make_shared<toolkit::Timer>(
            interval_sec,
            [this]() {
                collect();
                return true;
            },
            _poller);
    }
    virtual ~MetricCollector() { _timer.reset(); };

    /**
     * Set callback when value change
     */
    void setOnCollect(const std::function<void(T &)> &cb) { _on_collect = std::move(cb); }

protected:
    virtual void collect() = 0;
    
    void onCollect(T &value) {
        if (_on_collect) {
            _on_collect(value);
        }
    }

protected:
    toolkit::Timer::Ptr _timer;
    toolkit::EventPoller::Ptr _poller;
    std::function<void(T &)> _on_collect;
};

class ResourceMonitor : public std::enable_shared_from_this<ResourceMonitor> {
public:
    using Ptr = std::shared_ptr<ResourceMonitor>;

    explicit ResourceMonitor(toolkit::EventPoller::Ptr poller = nullptr) {
        _poller = poller ? poller : toolkit::EventPollerPool::Instance().getPoller();
    }

    virtual ~ResourceMonitor() =    default;

    virtual void start() = 0;

protected:
    std::mutex _mtx;
    toolkit::EventPoller::Ptr _poller;
};

} // namespace managerkit

#endif  // SERVER_RESOUCEMONITOR_H