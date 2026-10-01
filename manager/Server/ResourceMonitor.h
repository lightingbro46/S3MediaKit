#ifndef SERVER_RESOUCEMONITOR_H
#define SERVER_RESOUCEMONITOR_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include "Poller/Timer.h"
#include "Common/config.h"

namespace managerkit {

std::string format_bytes_human_readable(uint64_t bytes);

std::string format_double_2f(double value);

std::string format_float_2f(float value);

std::string format_duration_verbose(int64_t milliseconds);

std::string sanitize_for_json(double val);

std::string sanitize_for_json(float val);

template <typename T>
class MetricCollector {
public:
    using Ptr = std::shared_ptr<MetricCollector>;

    MetricCollector(toolkit::EventPoller::Ptr poller, float interval_sec = 10.0f) : _poller(poller) {
        _alive = std::make_shared<std::atomic<bool>>(true);
        auto alive = _alive;
        _timer = std::make_shared<toolkit::Timer>(
            interval_sec,
            [this, alive]() {
                if (!alive->load(std::memory_order_acquire)) return false;
                collect();
                return true;
            },
            _poller);
    }
    virtual ~MetricCollector() {
        _alive->store(false, std::memory_order_release);
        _timer.reset();
    };

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
    std::shared_ptr<std::atomic<bool>> _alive;
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
    DISK_IO = 5,
};

std::string getResourceTypeString(const ResourceType &type);

/**
 * Turns per-sample levels (0 normal, 1 warning, 2 critical) into alerts. A level change needs
 * `sustain` consecutive samples on the same side of the current level and settles on the mildest
 * (rising) or worst (falling) level among them. While above normal the alert repeats every
 * repeat_ms (<= 0: never). Not thread-safe: the owner serializes calls.
 */
struct AlertLevelTracker {
    int level = 0;
    int64_t last_emit_ms = 0;

    // True when an alert for `level` must be emitted now; level 0 then means "recovered".
    bool advance(int observed, int64_t now_ms, int sustain, int64_t repeat_ms);

private:
    int _up_count = 0;
    int _up_level = 0;
    int _down_count = 0;
    int _down_level = 0;
};

int64_t steadyNowMs();

class ResourceMonitor {
public:
    using Ptr = std::shared_ptr<ResourceMonitor>;

    explicit ResourceMonitor(const ResourceType type, toolkit::EventPoller::Ptr poller = nullptr) : _type(type) {
        _poller = poller ? poller : toolkit::EventPollerPool::Instance().getPoller();
        _monitor_alive = std::make_shared<std::atomic<bool>>(true);
    }

    virtual ~ResourceMonitor() {
        _monitor_alive->store(false, std::memory_order_release);
    }

    void setThreshold(float warning_threshold = -1.0f, float critical_threshold = -1.0f);

    std::pair<float, float> getThreshold();

    // sustain_samples: consecutive samples at a new level before it is reported (CPU/RAM sample every 10 s).
    // repeat_sec: repeat while above a threshold (<= 0: never).
    void setAlertPolicy(int sustain_samples, int repeat_sec);

    // Tests replace the steady clock used for repeats.
    void setAlertClock(const std::function<int64_t()> &now_ms);

private:
    virtual void start() = 0;

protected:
    // kBroadcastSystemAlert on a sustained level change and on repeat; kBroadcastSystemAlertRecovered
    // when it returns to normal.
    void emitSystemAlert(float usage);

    int alertRepeatSec() const { return _alert_repeat_sec; }
    int64_t alertNowMs() const;

protected:
    std::shared_ptr<std::atomic<bool>> _monitor_alive;
    std::mutex _mtx;
    ResourceType _type;
    toolkit::EventPoller::Ptr _poller;
    float _warning_threshold = -1.0f;
    float _critical_threshold = -1.0f;

private:
    std::mutex _alert_mtx;
    AlertLevelTracker _alert;
    std::atomic<int> _alert_sustain_samples{1};
    std::atomic<int> _alert_repeat_sec{300};
    std::function<int64_t()> _alert_clock;
};

} // namespace managerkit

#endif  // SERVER_RESOUCEMONITOR_H