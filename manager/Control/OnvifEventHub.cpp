#include <algorithm>
#include "Util/logger.h"
#include "Util/util.h"
#include "Util/NoticeCenter.h"
#include "Thread/WorkThreadPool.h"
#include "Common/config.h"
#include "Control/OnvifDeviceControl.h"
#include "Control/OnvifEventHub.h"
#include "Control/OnvifEventSubscriber.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

static string joinItems(const OnvifSimpleItems &items) {
    string ret;
    for (auto &item : items) {
        if (!ret.empty()) {
            ret += ',';
        }
        ret += item.first + "=" + item.second;
    }
    return ret;
}

OnvifEventHub &OnvifEventHub::Instance() {
    // Never destroyed: subscribers may still release slots while static objects go away at exit.
    static OnvifEventHub *s_instance = new OnvifEventHub();
    return *s_instance;
}

OnvifEventHub::OnvifEventHub() {
    NoticeCenter::Instance().addListener(this, kBroadcastOnvifEvent, [](BroadcastOnvifEventArgs) {
        OnvifEventHub::Instance().onEvent(device_id, address, event, lag_ms);
    });
    NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastReloadConfig, [](BroadcastReloadConfigArgs) {
        OnvifEventHub::Instance().onReload();
    });
    // The first line comes one interval after the first subscriber; the period is read each time.
    _stats_timer = WorkThreadPool::Instance().getPoller()->doDelayTask(60 * 1000, []() -> uint64_t {
        OnvifEventHub::Instance().logStats();
        GET_CONFIG(int, interval_sec, OnvifEventCfg::kStatsIntervalSec);
        return static_cast<uint64_t>(interval_sec > 0 ? interval_sec : 60) * 1000;
    });
}

void OnvifEventHub::add(const shared_ptr<OnvifEventSubscriber> &subscriber) {
    lock_guard<mutex> lck(_mtx);
    _subscribers.erase(std::remove_if(_subscribers.begin(), _subscribers.end(),
                                      [](const weak_ptr<OnvifEventSubscriber> &w) { return w.expired(); }),
                       _subscribers.end());
    _subscribers.push_back(subscriber);
}

vector<shared_ptr<OnvifEventSubscriber>> OnvifEventHub::subscribers() {
    vector<shared_ptr<OnvifEventSubscriber>> ret;
    lock_guard<mutex> lck(_mtx);
    for (auto &w : _subscribers) {
        if (auto s = w.lock()) {
            ret.push_back(std::move(s));
        }
    }
    return ret;
}

void OnvifEventHub::acquireSlot(const function<void()> &cb) {
    GET_CONFIG(int, max_in_flight, OnvifEventCfg::kMaxConcurrentSubscribe);
    {
        lock_guard<mutex> lck(_mtx);
        if (_in_flight >= std::max(max_in_flight, 1)) {
            _waiters.push_back(cb);
            return;
        }
        ++_in_flight;
    }
    cb();
}

void OnvifEventHub::releaseSlot() {
    function<void()> next;
    {
        lock_guard<mutex> lck(_mtx);
        if (_waiters.empty()) {
            if (_in_flight > 0) {
                --_in_flight;
            }
            return;
        }
        // The slot passes straight to the next waiter.
        next = std::move(_waiters.front());
        _waiters.pop_front();
    }
    next();
}

void OnvifEventHub::onReload() {
    for (auto &sub : subscribers()) {
        std::weak_ptr<OnvifEventSubscriber> weak_sub = sub;
        // A subscriber belongs to the poller of its camera.
        sub->poller()->async([weak_sub]() {
            if (auto s = weak_sub.lock()) {
                s->reconcile();
            }
        }, false);
    }
}

void OnvifEventHub::onEvent(const string &device_id, const string &address, const OnvifEventMessage &event,
                            int64_t lag_ms) {
    GET_CONFIG(int, per_min, OnvifEventCfg::kLogPerCameraPerMin);
    {
        const uint64_t minute = getCurrentMillisecond() / 60000;
        lock_guard<mutex> lck(_mtx);
        auto &bucket = _log_buckets[device_id];
        if (bucket.minute != minute) {
            bucket.minute = minute;
            bucket.lines = 0;
        }
        if (per_min > 0 && bucket.lines >= per_min) {
            ++bucket.dropped;
            return;
        }
        ++bucket.lines;
    }
    _StrPrinter line;
    line << "OnvifEvent " << device_id << " " << address << " " << event.eventType << " "
         << (event.propertyOp.empty() ? "Event" : event.propertyOp) << " " << event.topic << " cam=" << event.utcTime
         << " lag=" << lag_ms << "ms src{" << joinItems(event.source) << "} data{" << joinItems(event.data) << "}";
    if (event.propertyOp == "Initialized") {
        // The state a camera reports right after subscribing, not something that just happened.
        DebugL << line;
    } else {
        InfoL << line;
    }
}

void OnvifEventHub::logStats() {
    GET_CONFIG(int, interval_sec, OnvifEventCfg::kStatsIntervalSec);
    auto subs = subscribers();
    if (interval_sec <= 0 || subs.empty()) {
        return;
    }
    int counts[7] = { 0 };
    uint64_t events = 0;
    vector<pair<uint64_t, string>> top;
    uint64_t dropped = 0;
    {
        lock_guard<mutex> lck(_mtx);
        for (auto &sub : subs) {
            ++counts[static_cast<int>(sub->state())];
            auto total = sub->eventCount();
            auto &last = _last_counts[sub->deviceId()];
            auto delta = total >= last ? total - last : total;
            last = total;
            events += delta;
            if (delta) {
                top.emplace_back(delta, sub->deviceId());
            }
        }
        for (auto &it : _log_buckets) {
            dropped += it.second.dropped;
            it.second.dropped = 0;
        }
    }
    if (counts[static_cast<int>(OnvifEventSubscriber::State::Idle)] == static_cast<int>(subs.size()) && !events) {
        return;
    }
    std::sort(top.begin(), top.end(), [](const pair<uint64_t, string> &a, const pair<uint64_t, string> &b) {
        return a.first > b.first;
    });
    _StrPrinter line;
    line << "OnvifEvent stats:";
    for (int i = 0; i < 7; ++i) {
        if (counts[i]) {
            line << " " << OnvifEventSubscriber::stateName(static_cast<OnvifEventSubscriber::State>(i)) << "=" << counts[i];
        }
    }
    line << " events=" << events << " dropped_log=" << dropped << " top=[";
    for (size_t i = 0; i < top.size() && i < 5; ++i) {
        line << (i ? "," : "") << top[i].second << ":" << top[i].first;
    }
    line << "]";
    InfoL << line;
}

} // namespace managerkit
