#include "TimeScheduler.h"

namespace managerkit {

std::chrono::system_clock::time_point next_full_hour_sys() {
    using clock = std::chrono::system_clock;

    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);

    std::tm tm = *std::gmtime(&t);   // tránh DST
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_hour += 1;

    return clock::from_time_t(std::mktime(&tm));
}

std::chrono::steady_clock::time_point to_steady(std::chrono::system_clock::time_point target_sys) {
    auto now_sys = std::chrono::system_clock::now();
    auto now_steady = std::chrono::steady_clock::now();

    return now_steady + (target_sys - now_sys);
}

} // namespace managerkit
