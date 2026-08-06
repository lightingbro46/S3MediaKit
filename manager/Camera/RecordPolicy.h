#ifndef EXTENSION_RECORDPOLICY_H
#define EXTENSION_RECORDPOLICY_H

#include <string>
#include <memory>
#include <array>
#include <atomic>
#include <unordered_map>
#include "Poller/Timer.h"
#include "Common/DeviceSource.h"

namespace managerkit {
    
enum class RecordMode : uint8_t {
    NoRecord = 0,
    RecordLowResAndMotion,
    RecordOnlyMotion,
    RecordAlways,
    RecordModeMax
};
std::string getRecordModeString(RecordMode mode);

enum class ImageQuality : uint8_t {
    Low = 0,
    Medium,
    High,
    QualityMax
};
std::string getImageQualityString(ImageQuality quality);

struct RecordScheduleItem {
    int day = 0; // 0 = Monday, 1 = Tuesday, ... 6 = Sunday
    int hour = 0; // 0-23
    RecordMode mode = RecordMode::NoRecord;
    int fps = 0;
    ImageQuality q = ImageQuality::Low;

    bool operator==(const RecordScheduleItem &other) const {
        return day == other.day && 
               hour == other.hour && 
               mode == other.mode;
    }

    bool operator!=(const RecordScheduleItem &other) const {
        return !(*this == other);
    }
};

enum class RecordEventType : uint8_t {
    Unknown = 0,
    Motion,
};
std::string getRecordEventTypeString(RecordEventType type);

class RecordScheduler : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<RecordScheduler> {
public:
    using Ptr = std::shared_ptr<RecordScheduler>;
    using RecordScheduleMap = std::unordered_map<std::string, RecordScheduleItem>;

    static RecordScheduler::Ptr create(const DeviceTuple &tuple, bool start, const std::string &profile, const toolkit::EventPoller::Ptr &poller);

    RecordScheduler(const DeviceTuple &tuple, bool start, const std::string &profile, const toolkit::EventPoller::Ptr &poller);
    ~RecordScheduler();

    void setListener(const std::shared_ptr<DeviceSourceEvent> &delegate);

    std::string getProfile() const { return _profile; }

    void createTimer();

    void stopTimer();

    bool setupRecordEvent(RecordEventType type, bool start);

    bool isEventActive() const { return _event_active; }

    static std::unordered_map<std::string, RecordScheduleItem> parseRecordScheduleStr(const std::string &str);

private:
    void check(time_t time_now);

    void onSchedulerChange(RecordScheduleItem &item);

    int getRecordScheduledActive(time_t time) const;

private:
    DeviceTuple _tuple;
    std::string _profile;
    toolkit::EventPoller::Ptr _poller;
    toolkit::EventPoller::DelayTask::Ptr _delay_task;
    std::array<RecordScheduleItem, 7 * 24> _items_by_hour;
    int _active_index = -1;
    RecordScheduleItem _active_item;
    bool _has_active_item = false;

    // running
    std::atomic<bool> _running { false };
    bool _event_active = false;
    uint64_t _last_sink_time = 0;
    uint64_t _last_control_time = 0;
};

} // namespace managerkit

#endif // EXTENSION_RECORDPOLICY_H
