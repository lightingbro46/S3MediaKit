#ifndef CONTROL_ONVIFEVENTHUB_H
#define CONTROL_ONVIFEVENTHUB_H

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "Poller/EventPoller.h"
#include "Onvif/OnvifEvent.h"
#include "Control/OnvifEventKafka.h"

namespace managerkit {

class OnvifEventSubscriber;

/**
 * Node-wide side of the ONVIF event subscriptions: the limit on subscriptions being created at once
 * (onvif.eventMaxConcurrentSubscribe), the event bus publisher and the default event logger (listeners
 * of kBroadcastOnvifEvent, the log rate limited per camera), the periodic statistics line, and
 * onvif.eventEnable reloads.
 * Any thread.
 */
class OnvifEventHub {
public:
    static OnvifEventHub &Instance();

    void add(const std::shared_ptr<OnvifEventSubscriber> &subscriber);

    // `cb` runs once a slot is free (at once, or on the thread releasing one); its owner then calls
    // releaseSlot() exactly once.
    void acquireSlot(const std::function<void()> &cb);
    void releaseSlot();

private:
    OnvifEventHub();
    void onEvent(const std::string &device_id, const std::string &address, const mediakit::OnvifEventMessage &event,
                 int64_t lag_ms);
    // Event bus (Kafka): Initialized dropped or resent as a resync (OnvifEventKafka::decide).
    void publish(const std::string &device_id, const mediakit::OnvifEventMessage &event, int64_t lag_ms);
    void onReload();
    void logStats();
    std::vector<std::shared_ptr<OnvifEventSubscriber>> subscribers();

private:
    std::mutex _mtx;
    int _in_flight = 0;
    std::deque<std::function<void()>> _waiters;
    std::vector<std::weak_ptr<OnvifEventSubscriber>> _subscribers;

    struct LogBucket {
        uint64_t minute = 0;
        int lines = 0;
        uint64_t dropped = 0; // lines not written since the last statistics line
    };
    std::unordered_map<std::string, LogBucket> _log_buckets;
    std::unordered_map<std::string, uint64_t> _last_counts; // event count at the last statistics line
    // Per camera: state values the event bus took last (what the consumer holds).
    std::unordered_map<std::string, OnvifEventKafka::SentStates> _sent_states;
    toolkit::EventPoller::DelayTask::Ptr _stats_timer;
};

} // namespace managerkit

#endif // CONTROL_ONVIFEVENTHUB_H
