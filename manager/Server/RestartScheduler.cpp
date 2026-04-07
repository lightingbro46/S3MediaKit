#include "RestartScheduler.h"
#include "Common/config.h"
#include "Util/NoticeCenter.h"
#include "Util/logger.h"
#include <unordered_map>
#include "User/UserAuditLog.h"

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

    if (cfg.emitEvent) {
        emitEvent();
    }

    WarnL << "RestartScheduler: triggering system restart (type=" << cfg.type << ")";
    NOTICE_EMIT(BroadcastRestartServerArgs, Broadcast::kBroadcastRestartServer);
}

/**
 * Determine whether the server should restart at the current time.
 *
 * The actual trigger time = scheduled time + restartDelaySec (second precision).
 * This intentional delay lets the API service (which restarts at the
 * scheduled time) finish starting up before the media server restarts.
 *
 * Example: scheduled 02:00, restartDelaySec=90 → trigger at 02:01:30.
 *
 * Supported types:
 *   WEEKLY  – specific day-of-week + HH:MM + delay
 *   DAILY   – every day at HH:MM + delay
 *   HOURLY  – every N hours, at minute 0 + delay
 */
bool RestartScheduler::shouldRestartNow(const tm &now) const {
    // Parse "HH:MM" string into total seconds since midnight.
    auto parseTimeSec = [](const string &t, int &out_total_sec) -> bool {
        if (t.size() < 5 || t[2] != ':') return false;
        try {
            int hour = stoi(t.substr(0, 2));
            int min  = stoi(t.substr(3, 2));
            if (hour < 0 || hour >= 24 || min < 0 || min >= 60) return false;
            out_total_sec = hour * 3600 + min * 60;
        } catch (...) { return false; }
        return true;
    };

    // Apply restartDelaySec to a base second-of-day, wrapping at midnight.
    // Returns target {hour, min, sec}.
    const int delay_sec = _cfg.restartDelaySec;
    auto applyDelay = [delay_sec](int base_sec,
                                  int &out_hour, int &out_min, int &out_sec) {
        int total = (base_sec + delay_sec) % 86400;  // wrap at 24 h
        out_hour  = total / 3600;
        out_min   = (total % 3600) / 60;
        out_sec   = total % 60;
    };

    if (_cfg.type == "WEEKLY") {
        int target_dow = parseDayOfWeek(_cfg.dayOfWeek);
        if (target_dow < 0) return false;

        int base_sec = 0;
        if (!parseTimeSec(_cfg.time, base_sec)) return false;

        int th, tm_, ts;
        applyDelay(base_sec, th, tm_, ts);

        return now.tm_wday == target_dow
            && now.tm_hour == th
            && now.tm_min  == tm_
            && now.tm_sec  == ts;

    } else if (_cfg.type == "DAILY") {
        int base_sec = 0;
        if (!parseTimeSec(_cfg.time, base_sec)) return false;

        int th, tm_, ts;
        applyDelay(base_sec, th, tm_, ts);

        return now.tm_hour == th
            && now.tm_min  == tm_
            && now.tm_sec  == ts;

    } else if (_cfg.type == "HOURLY") {
        if (_cfg.everyHours.empty()) return false;
        int every = 0;
        try { every = stoi(_cfg.everyHours); } catch (...) { return false; }
        if (every <= 0) return false;

        // Base is second 0 of every N-th hour; apply delay on top.
        int th, tm_, ts;
        applyDelay(0, th, tm_, ts);  // base = 00:00:00 within the hour

        return (now.tm_hour % every == 0)
            && now.tm_min  == tm_
            && now.tm_sec  == ts;
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

void RestartScheduler::emitEvent() {
    auto flag = NOTICE_EMIT(BroadcastSystemAuditLogArgs, Broadcast::kBroadcastSystemAuditLog, SystemAuditLogType::MEDIA_SERVER_SHUTTING_DOWN_CONFIG, "Restart due to scheduled task");
    if (!flag) {
        WarnL << "Nobody listen on kBroadcastSystemAuditLog";
    }
}

} // namespace managerkit
