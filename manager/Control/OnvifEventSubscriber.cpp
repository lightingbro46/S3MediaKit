#include <algorithm>
#include <random>
#include "Util/logger.h"
#include "Util/util.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"
#include "Control/OnvifDeviceControl.h"
#include "Control/OnvifEventHub.h"
#include "Control/OnvifEventSubscriber.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

const string kBroadcastOnvifEvent = "kBroadcastOnvifEvent";

namespace {

class SoapEventTransport : public OnvifEventTransport {
public:
    void send(const string &url, const string &action, const string &envelope, float timeout_sec, const string &user,
              const string &pwd, const HttpAuthSession::Ptr &auth, const Callback &cb) override {
        // Quiet: the subscriber logs once per state change, not once per failed request.
        SoapUtil::sendSoapRequest(url, "\"" + action + "\"", envelope, cb, timeout_sec, user, pwd, true, auth);
    }
};

int randomBetween(int low, int high) {
    if (high <= low) {
        return low;
    }
    static thread_local std::mt19937 rng(std::random_device{}());
    return std::uniform_int_distribution<int>(low, high)(rng);
}

// One request's own deadline, not the pull: Create, Renew and Unsubscribe answer at once.
const float kRequestTimeoutSec = 10;

} // namespace

const char *OnvifEventSubscriber::stateName(State state) {
    switch (state) {
        case State::Idle: return "idle";
        case State::WaitSlot: return "wait_slot";
        case State::Subscribing: return "subscribing";
        case State::Pulling: return "pulling";
        case State::Renewing: return "renewing";
        case State::Backoff: return "backoff";
        case State::Suspended: return "suspended";
    }
    return "unknown";
}

OnvifEventSubscriber::Ptr OnvifEventSubscriber::create(const EventPoller::Ptr &poller, const string &device_id,
                                                       const OnvifEventTransport::Ptr &transport) {
    auto ret = std::make_shared<OnvifEventSubscriber>(poller, device_id,
                                                      transport ? transport : std::make_shared<SoapEventTransport>());
    OnvifEventHub::Instance().add(ret);
    return ret;
}

OnvifEventSubscriber::OnvifEventSubscriber(const EventPoller::Ptr &poller, const string &device_id,
                                           const OnvifEventTransport::Ptr &transport)
    : _poller(poller), _device_id(device_id), _transport(transport) {}

OnvifEventSubscriber::~OnvifEventSubscriber() {
    if (_timer) {
        _timer->cancel();
    }
    if (_holding_slot) {
        OnvifEventHub::Instance().releaseSlot();
    }
    // No Unsubscribe here: the owner stops the subscriber first (clearEndpoint), and a request sent
    // while the process exits could outlive the pollers. A forgotten subscription ends by itself
    // after onvif.eventTerminationSec.
}

bool OnvifEventSubscriber::shouldRun() const {
    // Read from mINI, not GET_CONFIG: on a reload, OnvifEventHub may call reconcile() before the
    // cached GET_CONFIG value of this function was refreshed.
    const bool event_enable = mINI::Instance()[OnvifEventCfg::kEventEnable].as<bool>();
    return event_enable && _camera_enabled && _has_endpoint && !_endpoint.filterTopics.empty();
}

void OnvifEventSubscriber::setEndpoint(const OnvifEventEndpoint &endpoint) {
    if (_has_endpoint && _running && _endpoint.sameTarget(endpoint) && _state != State::Suspended) {
        _endpoint.clockOffset = endpoint.clockOffset;
        return;
    }
    halt();
    if (!_has_endpoint || !_endpoint.sameTarget(endpoint)) {
        // Another account or camera: its Digest nonce means nothing here.
        _auth = std::make_shared<HttpAuthSession>();
    }
    _endpoint = endpoint;
    _has_endpoint = true;
    // A new connection may be a new firmware: ask for the filter and Renew again.
    _use_filter = true;
    _renew_unsupported = false;
    if (shouldRun()) {
        run();
    }
}

void OnvifEventSubscriber::clearEndpoint() {
    _has_endpoint = false;
    halt();
}

void OnvifEventSubscriber::setCameraEnabled(bool enabled) {
    if (_camera_enabled == enabled) {
        return;
    }
    _camera_enabled = enabled;
    reconcile();
}

void OnvifEventSubscriber::reconcile() {
    if (shouldRun()) {
        run();
    } else if (_running) {
        InfoL << "OnvifEvent " << _device_id << " stopped: event subscription turned off";
        halt();
    }
}

void OnvifEventSubscriber::run() {
    if (_running) {
        return;
    }
    _running = true;
    ++_generation;
    _backoff_sec = 0;
    _resubscribed_after_error = false;
    GET_CONFIG(int, jitter_ms, OnvifEventCfg::kStartJitterMs);
    setState(State::WaitSlot);
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    delay(randomBetween(0, std::max(0, jitter_ms)), [weak_self]() {
        if (auto self = weak_self.lock()) {
            self->waitSlot();
        }
    });
}

void OnvifEventSubscriber::halt() {
    if (!_running) {
        return;
    }
    _running = false;
    ++_generation;
    if (_timer) {
        _timer->cancel();
        _timer = nullptr;
    }
    releaseSlot();
    sendUnsubscribe();
    _states.clear();
    setState(State::Idle);
}

void OnvifEventSubscriber::setState(State state) {
    _state = state;
    _state_atomic.store(static_cast<int>(state));
}

void OnvifEventSubscriber::delay(int ms, const function<void()> &task) {
    if (_timer) {
        _timer->cancel();
    }
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    const uint64_t generation = _generation;
    _timer = _poller->doDelayTask(std::max(ms, 0), [weak_self, generation, task]() -> uint64_t {
        auto self = weak_self.lock();
        if (self && self->_generation == generation) {
            self->_timer = nullptr;
            task();
        }
        return 0;
    });
}

// ── Subscription slots ──

void OnvifEventSubscriber::waitSlot() {
    setState(State::WaitSlot);
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    const uint64_t generation = _generation;
    auto poller = _poller;
    OnvifEventHub::Instance().acquireSlot([weak_self, generation, poller]() {
        poller->async([weak_self, generation]() {
            auto self = weak_self.lock();
            if (!self) {
                OnvifEventHub::Instance().releaseSlot();
                return;
            }
            self->onSlot(generation);
        }, false);
    });
}

void OnvifEventSubscriber::onSlot(uint64_t generation) {
    if (generation != _generation || _holding_slot) {
        OnvifEventHub::Instance().releaseSlot();
        return;
    }
    _holding_slot = true;
    subscribe();
}

void OnvifEventSubscriber::releaseSlot() {
    if (_holding_slot) {
        _holding_slot = false;
        OnvifEventHub::Instance().releaseSlot();
    }
}

// ── Requests ──

void OnvifEventSubscriber::send(const string &url, const char *action, const string &body, bool addressed,
                                float timeout_sec, const function<void(const SoapObject &, const SoapErr &)> &cb) {
    string header;
    if (addressed) {
        header = OnvifEventRequest::addressingHeader(action, url, _subscription.referenceParameters);
    } else {
        header = OnvifEventRequest::addressingHeader(action, "", "");
    }
    auto envelope = SoapUtil::createSoapRequest(body, _endpoint.username, _endpoint.password, header,
                                                _endpoint.clockOffset);
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    const uint64_t generation = _generation;
    auto poller = _poller;
    _transport->send(url, action, envelope, timeout_sec, _endpoint.username, _endpoint.password, _auth,
                     [weak_self, generation, poller, cb](const SoapObject &node, const SoapErr &err) {
        poller->async([weak_self, generation, cb, node, err]() {
            auto self = weak_self.lock();
            if (self && self->_generation == generation) {
                cb(node, err);
            }
        }, false);
    });
}

void OnvifEventSubscriber::sendUnsubscribe() {
    if (_subscription.address.empty()) {
        return;
    }
    // Fire and forget: no callback reaches back to this object (it may be going away).
    auto header = OnvifEventRequest::addressingHeader(OnvifEventRequest::kUnsubscribeAction, _subscription.address,
                                                      _subscription.referenceParameters);
    auto envelope = SoapUtil::createSoapRequest(OnvifEventRequest::unsubscribe(), _endpoint.username,
                                                _endpoint.password, header, _endpoint.clockOffset);
    _transport->send(_subscription.address, OnvifEventRequest::kUnsubscribeAction, envelope, 3, _endpoint.username,
                     _endpoint.password, _auth, nullptr);
    _subscription = OnvifSubscriptionRef();
}

bool OnvifEventSubscriber::isAuthError(const SoapErr &err) {
    if (err.networkError()) {
        return false;
    }
    auto code = err.httpCode();
    return code == 401 || code == 403 || err.detail().find("NotAuthorized") != string::npos;
}

void OnvifEventSubscriber::subscribe() {
    setState(State::Subscribing);
    GET_CONFIG(int, termination_sec, OnvifEventCfg::kTerminationSec);
    GET_CONFIG(bool, filter_enable, OnvifEventCfg::kSubscribeFilter);
    const bool filtered = filter_enable && _use_filter;
    auto body = OnvifEventRequest::createPullPoint(filtered ? _endpoint.filterTopics : vector<string>(),
                                                   std::max(termination_sec, 10));
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    send(_endpoint.eventsUrl, OnvifEventRequest::kCreatePullPointAction, body, false, kRequestTimeoutSec,
         [weak_self, filtered](const SoapObject &node, const SoapErr &err) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        if (err && filtered && !err.networkError() && !isAuthError(err)) {
            // InvalidFilterFault, TopicExpressionDialectUnknownFault or a firmware that cannot parse
            // it: subscribe to everything, the server filters (same events, more traffic).
            WarnL << "OnvifEvent " << self->_device_id << " " << self->_endpoint.address
                  << " refused the topic filter (" << (err.detail().empty() ? std::to_string(err.httpCode()) : err.detail())
                  << "), subscribing without it";
            self->_use_filter = false;
            self->subscribe();
            return;
        }
        self->onSubscribed(self->_generation, node, err);
    });
}

void OnvifEventSubscriber::onSubscribed(uint64_t generation, const SoapObject &body, const SoapErr &err) {
    releaseSlot();
    if (err) {
        if (isAuthError(err)) {
            suspend("CreatePullPointSubscription rejected the credentials");
        } else {
            backoff(string("CreatePullPointSubscription failed: ") + (string)err);
        }
        return;
    }
    OnvifSubscriptionRef ref;
    if (!parseOnvifCreatePullPoint(body.as_xml(), ref)) {
        backoff("CreatePullPointSubscription answered without a subscription address");
        return;
    }
    _subscription = ref;
    updateTermination(ref.currentTime, ref.terminationTime);
    if (ref.currentTime) {
        _endpoint.clockOffset = ref.currentTime - time(nullptr);
    }
    InfoL << "OnvifEvent " << _device_id << " " << _endpoint.address << " subscribed " << ref.address << " filter="
          << (_use_filter ? std::to_string(_endpoint.filterTopics.size()) + " topics" : string("off"));
    _quick_empty_pulls = 0;
    setState(State::Pulling);
    pull();
}

void OnvifEventSubscriber::pull() {
    setState(State::Pulling);
    GET_CONFIG(int, timeout_sec, OnvifEventCfg::kPullTimeoutSec);
    GET_CONFIG(int, grace_sec, OnvifEventCfg::kPullGraceSec);
    GET_CONFIG(int, message_limit, OnvifEventCfg::kMessageLimit);
    const int pull_timeout = std::max(timeout_sec, 1);
    const uint64_t sent_ms = getCurrentMillisecond();
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    const uint64_t generation = _generation;
    send(_subscription.address, OnvifEventRequest::kPullMessagesAction,
         OnvifEventRequest::pullMessages(pull_timeout, std::max(message_limit, 1)), true,
         pull_timeout + std::max(grace_sec, 1), [weak_self, generation, sent_ms](const SoapObject &node, const SoapErr &err) {
        if (auto self = weak_self.lock()) {
            self->onPulled(generation, sent_ms, node, err);
        }
    });
}

void OnvifEventSubscriber::onPulled(uint64_t generation, uint64_t sent_ms, const SoapObject &body, const SoapErr &err) {
    if (err) {
        if (isAuthError(err)) {
            suspend("PullMessages rejected the credentials");
        } else if (err.networkError()) {
            // The camera is unreachable: the subscription cannot be cancelled, it expires by itself.
            _subscription = OnvifSubscriptionRef();
            backoff(string("PullMessages failed: ") + (string)err);
        } else {
            // ResourceUnknown, 400/404/500: the camera forgot the subscription (reboot) or broke it.
            if (!_resubscribed_after_error) {
                InfoL << "OnvifEvent " << _device_id << " " << _endpoint.address << " subscription lost ("
                      << (err.detail().empty() ? std::to_string(err.httpCode()) : err.detail()) << "), subscribing again";
                _resubscribed_after_error = true;
                resubscribe(false);
            } else {
                _subscription = OnvifSubscriptionRef();
                backoff(string("PullMessages failed: ") + (string)err);
            }
        }
        return;
    }
    OnvifPullResult result;
    if (!parseOnvifPullMessages(body.as_xml(), result)) {
        _subscription = OnvifSubscriptionRef();
        backoff("PullMessages answered without PullMessagesResponse");
        return;
    }
    _backoff_sec = 0;
    _resubscribed_after_error = false;
    if (result.currentTime) {
        _endpoint.clockOffset = result.currentTime - time(nullptr);
    }
    updateTermination(result.currentTime, result.terminationTime);
    handleMessages(result);

    if (needsRenew()) {
        renew();
        return;
    }
    // A camera ignoring Timeout answers an empty pull at once: never spin on it.
    const bool quick_empty = result.messages.empty() && getCurrentMillisecond() - sent_ms < 1000;
    _quick_empty_pulls = quick_empty ? _quick_empty_pulls + 1 : 0;
    if (_quick_empty_pulls >= 3) {
        std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
        delay(1000, [weak_self]() {
            if (auto self = weak_self.lock()) {
                self->pull();
            }
        });
        return;
    }
    pull();
}

void OnvifEventSubscriber::renew() {
    if (_renew_unsupported) {
        resubscribe(true);
        return;
    }
    setState(State::Renewing);
    GET_CONFIG(int, termination_sec, OnvifEventCfg::kTerminationSec);
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    const uint64_t generation = _generation;
    send(_subscription.address, OnvifEventRequest::kRenewAction, OnvifEventRequest::renew(std::max(termination_sec, 10)),
         true, kRequestTimeoutSec, [weak_self, generation](const SoapObject &node, const SoapErr &err) {
        if (auto self = weak_self.lock()) {
            self->onRenewed(generation, node, err);
        }
    });
}

void OnvifEventSubscriber::onRenewed(uint64_t generation, const SoapObject &body, const SoapErr &err) {
    if (err) {
        if (isAuthError(err)) {
            suspend("Renew rejected the credentials");
        } else if (err.networkError()) {
            _subscription = OnvifSubscriptionRef();
            backoff(string("Renew failed: ") + (string)err);
        } else {
            InfoL << "OnvifEvent " << _device_id << " " << _endpoint.address
                  << " does not renew subscriptions, subscribing again when one ends";
            _renew_unsupported = true;
            resubscribe(true);
        }
        return;
    }
    time_t current_time = 0, termination_time = 0;
    if (!parseOnvifRenew(body.as_xml(), current_time, termination_time)) {
        _renew_unsupported = true;
        resubscribe(true);
        return;
    }
    if (!termination_time) {
        // Some cameras answer an empty RenewResponse: assume the requested duration.
        updateTermination(0, 0);
    } else {
        updateTermination(current_time, termination_time);
    }
    pull();
}

void OnvifEventSubscriber::resubscribe(bool unsubscribe) {
    if (unsubscribe) {
        sendUnsubscribe();
    } else {
        _subscription = OnvifSubscriptionRef();
    }
    _states.clear();
    waitSlot();
}

void OnvifEventSubscriber::backoff(const string &why) {
    GET_CONFIG(int, retry_max_sec, OnvifEventCfg::kRetryMaxSec);
    const int max_sec = std::max(retry_max_sec, 5);
    const bool first = _backoff_sec == 0;
    _backoff_sec = first ? 5 : std::min(_backoff_sec * 2, max_sec);
    const int delay_ms = randomBetween(_backoff_sec * 800, _backoff_sec * 1200);
    if (first) {
        WarnL << "OnvifEvent " << _device_id << " " << _endpoint.address << " " << why;
    } else {
        DebugL << "OnvifEvent " << _device_id << " " << _endpoint.address << " " << why;
    }
    _states.clear();
    setState(State::Backoff);
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    delay(delay_ms, [weak_self]() {
        if (auto self = weak_self.lock()) {
            self->waitSlot();
        }
    });
}

void OnvifEventSubscriber::suspend(const string &why) {
    // No more login attempts than the controller makes: a camera may lock the address after a few
    // rejected logins. The next connect of the controller starts it again, or this long wait.
    GET_CONFIG(int, retry_max_sec, OnvifEventCfg::kRetryMaxSec);
    WarnL << "OnvifEvent " << _device_id << " " << _endpoint.address << " suspended: " << why;
    _subscription = OnvifSubscriptionRef();
    _states.clear();
    setState(State::Suspended);
    std::weak_ptr<OnvifEventSubscriber> weak_self = shared_from_this();
    delay(std::max(retry_max_sec, 5) * 1000, [weak_self]() {
        if (auto self = weak_self.lock()) {
            self->waitSlot();
        }
    });
}

// ── Messages ──

void OnvifEventSubscriber::updateTermination(time_t current_time, time_t termination_time) {
    GET_CONFIG(int, termination_sec, OnvifEventCfg::kTerminationSec);
    int64_t remaining_sec = std::max(termination_sec, 10);
    // Camera clock against camera clock: right even when the camera clock is off.
    if (current_time && termination_time && termination_time > current_time) {
        remaining_sec = termination_time - current_time;
    }
    _expire_ms = getCurrentMillisecond() + remaining_sec * 1000;
}

bool OnvifEventSubscriber::needsRenew() const {
    GET_CONFIG(int, margin_sec, OnvifEventCfg::kRenewMarginSec);
    GET_CONFIG(int, timeout_sec, OnvifEventCfg::kPullTimeoutSec);
    GET_CONFIG(int, grace_sec, OnvifEventCfg::kPullGraceSec);
    // The next pull may hold the camera for its whole timeout: renew before starting one that could
    // outlive the subscription.
    const uint64_t lead_ms = static_cast<uint64_t>(std::max(margin_sec, 0) + std::max(timeout_sec, 1) + std::max(grace_sec, 1)) * 1000;
    return getCurrentMillisecond() + lead_ms >= _expire_ms;
}

void OnvifEventSubscriber::handleMessages(const OnvifPullResult &result) {
    const time_t camera_now = time(nullptr) + _endpoint.clockOffset;
    for (auto &msg : result.messages) {
        if (msg.eventType.empty()) {
            // Not normalized: a camera ignoring the filter, or no filter.
            TraceL << "OnvifEvent " << _device_id << " ignored topic " << msg.topic;
            continue;
        }
        if (!msg.propertyOp.empty()) {
            auto key = msg.stateKey();
            if (msg.propertyOp == "Deleted") {
                if (!_states.erase(key)) {
                    continue;
                }
            } else {
                auto value = msg.stateValue();
                auto it = _states.find(key);
                if (it != _states.end() && it->second == value) {
                    // Same state again (MotionAlarm and CellMotionDetector of one source, a repeat).
                    continue;
                }
                _states[key] = value;
            }
        }
        _event_count.fetch_add(1);
        const int64_t lag_ms = msg.cameraUtc ? static_cast<int64_t>(camera_now - msg.cameraUtc) * 1000 : 0;
        NOTICE_EMIT(BroadcastOnvifEventArgs, kBroadcastOnvifEvent, _device_id, _endpoint.address, msg, lag_ms);
    }
}

} // namespace managerkit
