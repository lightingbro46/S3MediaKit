#ifndef EXTENSION_RECORDPOLICY_H
#define EXTENSION_RECORDPOLICY_H

#include <string>
#include <memory>
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
    int day = 0; // 0 = Sunday, 1 = Monday, ... 6 = Saturday
    int hour = 0; // 0-23
    RecordMode mode = RecordMode::NoRecord;
    int fps = 0;
    ImageQuality q = ImageQuality::Low;
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

    static RecordScheduler::Ptr create(const DeviceTuple &tuple, const std::string &profile, const toolkit::EventPoller::Ptr &poller);

    RecordScheduler(const DeviceTuple &tuple, const std::string &profile, const toolkit::EventPoller::Ptr &poller);
    ~RecordScheduler();

    void setListener(const std::shared_ptr<DeviceSourceEvent> &delegate);

    std::string getProfile() const { return _profile; }

    void createTimer();

    void stopTimer();

    bool setupRecordEvent(RecordEventType type, bool start);

    bool isEventActive() const { return _event_active; }

    static std::unordered_map<std::string, RecordScheduleItem> parseRecordScheduleStr(const std::string &str);

private:
    void onSchedulerChange(RecordScheduleItem &item);

    RecordScheduleMap::iterator getRecordScheduledActive(time_t time);

private:
    DeviceTuple _tuple;
    std::string _profile;
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
    RecordScheduleMap _items;
    RecordScheduleMap::iterator _it = _items.end();

    // running
    bool _running = false;
    bool _event_active = false;
    uint64_t _last_sink_time = 0;
    uint64_t _last_control_time = 0;
};

} // namespace managerkit

#endif // EXTENSION_RECORDPOLICY_H