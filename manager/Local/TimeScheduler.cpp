#include "Util/logger.h"
#include "TimeScheduler.h"

using namespace std;

namespace managerkit {

WeekTime getWeekTime(uint64_t stamp) {
    time_t ts = (time_t)stamp;
    std::tm *lt = std::localtime(&ts);
    WeekTime wt;
    wt.day_of_week = lt->tm_wday; // day of week
    wt.hour = lt->tm_hour;        // hour of day
    return wt;
}

} // namespace managerkit
