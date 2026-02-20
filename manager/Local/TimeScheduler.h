#ifndef LOCAL_TIMESCHEDULER_H
#define LOCAL_TIMESCHEDULER_H

#include <thread>
#include <atomic>
#include "Util/logger.h"

namespace managerkit {

class IScheduledJob {
public:
    virtual ~IScheduledJob() = default;
    virtual void execute() = 0;
};

std::chrono::system_clock::time_point next_full_hour_sys();

std::chrono::steady_clock::time_point to_steady(std::chrono::system_clock::time_point target_sys);

/**
 * Hourly scheduler
 * Save configuration according to the hourly time
 */
class HourlyScheduler {
public:
    
    HourlyScheduler(IScheduledJob &job) : _job(job), _running(false) {}

    ~HourlyScheduler() { stop(); }

    void start() {
        if (_running) {
            return;
        }
        _running = true;
        _worker = std::thread(&HourlyScheduler::loop, this);
    }

    void stop() {
        _running = false;
        if (_worker.joinable())
            _worker.join();  
    }

private:
    void loop() {
        

        while (_running) {
            // 1. tính đầu giờ thật (system)
            auto next_sys = next_full_hour_sys();

            // 2. convert sang steady tại thời điểm hiện tại
            auto next_steady = to_steady(next_sys);

            // 3. ngủ bằng steady_clock (không drift)
            std::this_thread::sleep_until(next_steady);

            if (!_running) break;

            try {
                _job.execute();
            } catch (...) {
                WarnL << "HourlyScheduler Job failed";
            }
        }
    }

private:
    IScheduledJob &_job;
    std::atomic<bool> _running{false};
    std::thread _worker;
};

} // namespace managerkit

#endif // LOCAL_TIMESCHEDULER_H