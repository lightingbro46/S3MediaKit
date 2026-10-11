#ifndef CONTROL_ONVIFEVENTSUBSCRIBER_H
#define CONTROL_ONVIFEVENTSUBSCRIBER_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Poller/EventPoller.h"
#include "Onvif/OnvifEvent.h"
#include "Onvif/SoapUtil.h"

namespace managerkit {

// An ONVIF event of a camera, after the duplicate state changes were dropped. Emitted on the poller
// of the camera. `lag_ms`: server clock minus the camera event time (corrected by the clock offset).
extern const std::string kBroadcastOnvifEvent;
#define BroadcastOnvifEventArgs const std::string &device_id, const std::string &address, \
                                const mediakit::OnvifEventMessage &event, const int64_t &lag_ms

// Where and how to subscribe to the events of one camera.
struct OnvifEventEndpoint {
    std::string address;   // camera address, for logs
    std::string eventsUrl; // GetCapabilities Events.XAddr
    std::string username;
    std::string password;
    std::vector<std::string> filterTopics; // concrete topics of the supported topics (OnvifEventTopics)
    time_t clockOffset = 0;                // camera clock minus local clock, seconds

    // Same subscription target (the clock offset is not part of it).
    bool sameTarget(const OnvifEventEndpoint &o) const {
        return address == o.address && eventsUrl == o.eventsUrl && username == o.username
            && password == o.password && filterTopics == o.filterTopics;
    }
};

// Sends one SOAP request asynchronously; unit tests replace it.
class OnvifEventTransport {
public:
    using Ptr = std::shared_ptr<OnvifEventTransport>;
    // May run on any thread.
    using Callback = std::function<void(const SoapObject &body, const SoapErr &err)>;
    virtual ~OnvifEventTransport() = default;
    // `envelope` is a complete SOAP envelope; `user`/`pwd` answer an HTTP Digest challenge.
    virtual void send(const std::string &url, const std::string &action, const std::string &envelope,
                      float timeout_sec, const std::string &user, const std::string &pwd, const Callback &cb) = 0;
};

/**
 * PullPoint subscription of one camera (docs/camera-control-protocol.md, "Sự kiện ONVIF"). Lives on the poller of the camera:
 * every public method must be called there, and every callback comes back to it. No thread is held
 * while a pull waits on the camera.
 *
 * Runs while it has an endpoint (the ONVIF controller is connected and the camera has a supported
 * topic), the camera allows it (enableOnvifEvent) and onvif.eventEnable is on.
 */
class OnvifEventSubscriber : public std::enable_shared_from_this<OnvifEventSubscriber> {
public:
    using Ptr = std::shared_ptr<OnvifEventSubscriber>;

    enum class State { Idle, WaitSlot, Subscribing, Pulling, Renewing, Backoff, Suspended };
    static const char *stateName(State state);

    // Registers the subscriber with OnvifEventHub (statistics, onvif.eventEnable reloads).
    static Ptr create(const toolkit::EventPoller::Ptr &poller, const std::string &device_id,
                      const OnvifEventTransport::Ptr &transport = nullptr);
    ~OnvifEventSubscriber();

    // The controller connected (again). Same target while running: only the clock offset is taken.
    void setEndpoint(const OnvifEventEndpoint &endpoint);
    // The controller is gone: unsubscribe and stop.
    void clearEndpoint();
    // CameraOption::enableOnvifEvent.
    void setCameraEnabled(bool enabled);
    // Re-reads onvif.eventEnable.
    void reconcile();

    State state() const { return static_cast<State>(_state_atomic.load()); }
    const std::string &deviceId() const { return _device_id; }
    const toolkit::EventPoller::Ptr &poller() const { return _poller; }
    // Events emitted so far; any thread.
    uint64_t eventCount() const { return _event_count.load(); }

    OnvifEventSubscriber(const toolkit::EventPoller::Ptr &poller, const std::string &device_id,
                         const OnvifEventTransport::Ptr &transport);

private:
    bool shouldRun() const;
    void run();
    void halt();
    void setState(State state);

    void waitSlot();
    void onSlot(uint64_t generation);
    void releaseSlot();
    void subscribe();
    void onSubscribed(uint64_t generation, const SoapObject &body, const SoapErr &err);
    void pull();
    void onPulled(uint64_t generation, uint64_t sent_ms, const SoapObject &body, const SoapErr &err);
    void renew();
    void onRenewed(uint64_t generation, const SoapObject &body, const SoapErr &err);
    // Drops the current subscription (Unsubscribe when `unsubscribe`) and subscribes again.
    void resubscribe(bool unsubscribe);
    void backoff(const std::string &why);
    void suspend(const std::string &why);
    void sendUnsubscribe();

    void handleMessages(const mediakit::OnvifPullResult &result);
    void updateTermination(time_t current_time, time_t termination_time);
    bool needsRenew() const;

    // Sends `body` to `url`; `cb` runs on the poller unless the generation changed meanwhile.
    void send(const std::string &url, const char *action, const std::string &body, bool addressed, float timeout_sec,
              const std::function<void(const SoapObject &body, const SoapErr &err)> &cb);
    void delay(int ms, const std::function<void()> &task);

    static bool isAuthError(const SoapErr &err);

private:
    toolkit::EventPoller::Ptr _poller;
    std::string _device_id;
    OnvifEventTransport::Ptr _transport;

    bool _has_endpoint = false;
    OnvifEventEndpoint _endpoint;
    bool _camera_enabled = true;
    bool _running = false;
    // Incremented by run()/halt(): callbacks of an older generation are dropped.
    uint64_t _generation = 0;
    State _state = State::Idle;
    std::atomic<int> _state_atomic { static_cast<int>(State::Idle) };

    bool _holding_slot = false;
    mediakit::OnvifSubscriptionRef _subscription; // address empty = none
    bool _use_filter = true;                      // false once the camera refused the topic filter
    bool _renew_unsupported = false;
    bool _resubscribed_after_error = false;       // one immediate retry after a dead subscription
    uint64_t _expire_ms = 0;                      // steady ms when the subscription ends
    int _backoff_sec = 0;
    int _quick_empty_pulls = 0;
    toolkit::EventPoller::DelayTask::Ptr _timer;

    // Last value of each state (OnvifEventMessage::stateKey()).
    std::unordered_map<std::string, std::string> _states;
    std::atomic<uint64_t> _event_count { 0 };
};

} // namespace managerkit

#endif // CONTROL_ONVIFEVENTSUBSCRIBER_H
