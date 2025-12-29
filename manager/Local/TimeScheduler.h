#ifndef LOCAL_TIMESCHEDULER_H
#define LOCAL_TIMESCHEDULER_H

#include <string>
#include <memory>
#include <unordered_map>
#include "Poller/Timer.h" 

namespace managerkit {

/**
 * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD"
 * Chuỗi có định dạng "YYYY-MM-DD" sẽ được hiểu là "YYYY-MM-DD 00:00:00"
 * @param str Chuỗi ngày giờ
 * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
 */
uint64_t getTsFromDateStr(const std::string &str);

/**
 * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD/HH-MM-SS(-extra)"
 * @param str Chuỗi ngày giờ
 * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
 */
uint64_t getTsFromDateTimeStr(const std::string &str);

/**
 * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD/YYYY-MM-DD-HH-MM-SS(-extra)"
 * @param str Chuỗi ngày giờ
 * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
 */
uint64_t getTsFromDateTimeStr2(const std::string &str);

struct WeekTime {
    int day_of_week; // 0 = Sunday, 1 = Monday, ... 6 = Saturday
    int hour;        // 0-23
};

WeekTime getWeekTime(uint64_t stamp);

template <typename Type, typename Helper>
inline void parse_schedule_str(const std::string &input, std::unordered_map<int, std::unordered_map<int, Type>> &output_map) {
    for (int day = 0; day < 7; ++day) {
        auto &it_day = output_map[day];
        for (int hour = 0; hour < 24; ++hour) {
            char c = input[day * 24 + hour];
            output_map[day][hour] = Helper::fromChar(c);
        }
    }
}

/**
 * Time scheduler
 * Save configuration according to the weekly time,
 * suitable for the mode with reference value from 0 to 9
 */
template <typename Type, typename Helper>
class TimeScheduler : public std::enable_shared_from_this<TimeScheduler<Type, Helper>> {
public:
    using Ptr = std::shared_ptr<TimeScheduler<Type, Helper>>;
    using onChangeMode = std::function<void(Type &)>;

    TimeScheduler(const std::string &schedule_str) 
        : _schedule_str(schedule_str) {
        if (schedule_str.size() != 24 * 7) {
            WarnL << "Time scheduler string has invalid size:" << schedule_str.size() << ". Ignore";
            return;
        }

        parse_schedule_str<Type, Helper>(schedule_str, _scheduler_map);
        _mode = getModeActive();
    }

    ~TimeScheduler() {
        _timer_scheduler.reset();
    }

    Type getModeActive(uint64_t stamp = time(nullptr)) {
        auto wt = getWeekTime(stamp);
        return _scheduler_map[wt.day_of_week][wt.hour];
    }

    std::string getSchedulerString() { return _schedule_str; }

    void setOnChangeMode(const onChangeMode &cb) { _on_change_mode = std::move(cb); }

    void start(double interval_sec = 10.0f) {
        if (!_scheduler_map.size()) {
            return;
        }

        std::weak_ptr<TimeScheduler<Type, Helper>> weak_self = TimeScheduler<Type, Helper>::shared_from_this();
        _timer_scheduler = std::make_shared<toolkit::Timer>(
            interval_sec,
            [weak_self]() {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return false;
                }
                auto previous_mode = strong_self->_mode;
                strong_self->_mode = strong_self->getModeActive();

                if (previous_mode != strong_self->_mode) {
                    // call callback when mode change
                    if (strong_self->_on_change_mode) {
                        strong_self->_on_change_mode(strong_self->_mode);
                    }
                }
                return true;
            },
            nullptr);
        TraceL << "Time scheduler is set";
    }

private:
    std::string _schedule_str;
    std::unordered_map<int, std::unordered_map<int, Type>> _scheduler_map;
    toolkit::Timer::Ptr _timer_scheduler;
    Type _mode;
    onChangeMode _on_change_mode = nullptr;
};

} // namespace managerkit

#endif // LOCAL_TIMESCHEDULER_H