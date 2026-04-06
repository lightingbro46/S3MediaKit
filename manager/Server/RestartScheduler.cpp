#include "RestartScheduler.h"
#include "Common/config.h"
#include "Util/NoticeCenter.h"
#include "Util/logger.h"
#include <unordered_map>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

RestartScheduler::RestartScheduler(EventPoller::Ptr poller)
    : _poller(std::move(poller)) {}

RestartScheduler::~RestartScheduler() {
    stop();
}

void RestartScheduler::start() {
    if (_timer) {
        WarnL << "RestartScheduler is already running. Ignore";
        return;
    }
    weak_ptr<RestartScheduler> weak_self = shared_from_this();
    _timer = make_shared<Timer>(
        1.0f,
        [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            strong_self->onTick();
            return true;
        },
        _poller);
    InfoL << "RestartScheduler started";
}

void RestartScheduler::stop() {
    _timer.reset();
}

void RestartScheduler::setConfig(const RestartSchedulerConfig &cfg) {
    lock_guard<mutex> lock(_mtx);
    _cfg = cfg;
    InfoL << "RestartScheduler config updated"
          << " enabled=" << cfg.enabled
          << " type="    << cfg.type
          << " time="    << cfg.time
          << " dow="     << cfg.dayOfWeek
          << " every="   << cfg.everyHours;
}

RestartSchedulerConfig RestartScheduler::getConfig() const {
    lock_guard<mutex> lock(_mtx);
    return _cfg;
}

// ─── internal ───────────────────────────────────────────────────────────────

void RestartScheduler::onTick() {
    RestartSchedulerConfig cfg;
    {
        lock_guard<mutex> lock(_mtx);
        cfg = _cfg;
    }

    if (!cfg.enabled) {
        return;
    }

    time_t now_time = time(nullptr);
    tm now_tm;
    localtime_r(&now_time, &now_tm);

    if (!shouldRestartNow(now_tm)) {
        return;
    }

    // Guard: do not restart more than once per minute
    if (_last_restart != 0 && difftime(now_time, _last_restart) < 60.0) {
        return;
    }
    _last_restart = now_time;

    WarnL << "RestartScheduler: triggering system restart (type=" << cfg.type << ")";
    NOTICE_EMIT(BroadcastRestartServerArgs, Broadcast::kBroadcastRestartServer);
}

bool RestartScheduler::shouldRestartNow(const tm &now) const {
    // Parse HH:MM
    auto parseTime = [](const string &t, int &hour, int &min) -> bool {
        if (t.size() < 5 || t[2] != ':') return false;
        try {
            hour = stoi(t.substr(0, 2));
            min  = stoi(t.substr(3, 2));
        } catch (...) { return false; }
        return hour >= 0 && hour < 24 && min >= 0 && min < 60;
    };

    if (_cfg.type == "WEEKLY") {
        int target_dow = parseDayOfWeek(_cfg.dayOfWeek);
        if (target_dow < 0) return false;

        int hour = 0, min = 0;
        if (!parseTime(_cfg.time, hour, min)) return false;

        return now.tm_wday == target_dow
            && now.tm_hour == hour
            && now.tm_min  == min
            && now.tm_sec  == 0;

    } else if (_cfg.type == "DAILY") {
        int hour = 0, min = 0;
        if (!parseTime(_cfg.time, hour, min)) return false;

        return now.tm_hour == hour
            && now.tm_min  == min
            && now.tm_sec  == 0;

    } else if (_cfg.type == "HOURLY") {
        if (_cfg.everyHours.empty()) return false;
        int every = 0;
        try { every = stoi(_cfg.everyHours); } catch (...) { return false; }
        if (every <= 0) return false;

        // Fire at the start of every N-th hour
        return (now.tm_hour % every == 0)
            && now.tm_min  == 0
            && now.tm_sec  == 0;
    }

    return false;
}

int RestartScheduler::parseDayOfWeek(const string &dow) {
    static const unordered_map<string, int> kMap = {
        {"SUN", 0}, {"MON", 1}, {"TUE", 2}, {"WED", 3},
        {"THU", 4}, {"FRI", 5}, {"SAT", 6}
    };
    auto it = kMap.find(dow);
    return it != kMap.end() ? it->second : -1;
}

} // namespace managerkit
