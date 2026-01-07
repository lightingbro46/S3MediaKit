#ifndef SERVER_RESOUCEMONITOR_H
#define SERVER_RESOUCEMONITOR_H

#include <memory>
#include <mutex>
#include "Poller/Timer.h"
#include "Common/config.h"

namespace managerkit {

std::string format_bytes_human_readable(uint64_t bytes);

std::string format_double_2f(double value);

std::string formatDuration(int64_t milliseconds);

std::string sanitize_for_json(double val);

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

enum class ResourceType : uint8_t {
    CPU = 0,
    MEMORY = 1,
    NETWORK = 2,
    HDD = 3,
    READER = 4,
};

std::string getResourceTypeString(const ResourceType &type);

class ResourceMonitor {
public:
    using Ptr = std::shared_ptr<ResourceMonitor>;

    explicit ResourceMonitor(const ResourceType type, toolkit::EventPoller::Ptr poller = nullptr) : _type(type) {
        _poller = poller ? poller : toolkit::EventPollerPool::Instance().getPoller();
    }

    virtual ~ResourceMonitor() = default;

    void setThreshold(double warning_threshold = -1, double critical_threshold = -1);

    std::pair<double, double> getThreshold();

private:
    virtual void start() = 0;

protected:
    void emitSystemAlert(double usage);

protected:
    std::mutex _mtx;
    ResourceType _type;
    toolkit::EventPoller::Ptr _poller;
    double _warning_threshold = -1.0;
    double _critical_threshold = -1.0;
};

} // namespace managerkit

#endif  // SERVER_RESOUCEMONITOR_H