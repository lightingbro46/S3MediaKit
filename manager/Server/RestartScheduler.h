#ifndef S3MANAGERKIT_RESTARTSCHEDULER_H
#define S3MANAGERKIT_RESTARTSCHEDULER_H

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"

namespace managerkit {

struct RestartSchedulerConfig {
    bool        enabled    = false;
    std::string type;        // "WEEKLY" | "DAILY" | "HOURLY"
    std::string time;        // "HH:MM"  – used by WEEKLY and DAILY
    std::string dayOfWeek;   // "MON".."SUN" – used by WEEKLY
    std::string everyHours;  // numeric string, e.g. "6" – used by HOURLY
    std::string timezone;    // reserved, currently unused
    int  restartDelaySec = 60; // delay seconds before restart, default to 60
    bool emitEvent = true; // whether to emit event when restart is triggered, default to true
};

class RestartScheduler : public std::enable_shared_from_this<RestartScheduler> {
public:
    using Ptr = std::shared_ptr<RestartScheduler>;

    explicit RestartScheduler(toolkit::EventPoller::Ptr poller);
    ~RestartScheduler();

    void start();
    void stop();

    void setConfig(const RestartSchedulerConfig &cfg);
    RestartSchedulerConfig getConfig() const;

private:
    void onTick();
    bool shouldRestartNow(const std::tm &now) const;

    static int parseDayOfWeek(const std::string &dow);

    void emitEvent();

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr       _timer;

    mutable std::mutex   _mtx;
    RestartSchedulerConfig _cfg;
    std::time_t          _last_restart { 0 };
};

} // namespace managerkit

#endif // S3MANAGERKIT_RESTARTSCHEDULER_H
