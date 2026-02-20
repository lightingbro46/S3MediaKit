#ifndef EXTENSION_RECORDPOLICY_H
#define EXTENSION_RECORDPOLICY_H

#include <string>
#include "Poller/Timer.h"

namespace managerkit {
    
enum class RecordMode : uint8_t {
    NoRecord = 0,
    RecordOnlyMotion,
    RecordLowResAndMotion,
    RecordAlways,
    RecordModeMax
};

std::string getRecordModeString(RecordMode mode);

enum class RecordState : uint8_t {
    Idle = 0,
    Recording,
};

struct RecordScheduleItem {
    using Ptr = std::shared_ptr<RecordScheduleItem>;

    int day = 0; // 0 = Sunday, 1 = Monday, ... 6 = Saturday
    int hour = 0; // 0-23
    RecordMode mode = RecordMode::NoRecord;
    int fps = 0;
    std::string q;
};

class GenericRtspCameraImp; // forward declaration

class RecordingController : public std::enable_shared_from_this<RecordingController> {
public:
    using Ptr = std::shared_ptr<RecordingController>;
    using OnRecordModeChange = std::function<void(int type, bool start, bool archive, int backtime_ms)>;

    RecordingController(const toolkit::EventPoller::Ptr &poller = nullptr);
    ~RecordingController();

    void start();

    void setScheduleStr(const std::string &schedule_str);

    RecordScheduleItem::Ptr getRecordScheduledActive();

    void onRecordEvent(bool bActive, uint64_t pre_ms);

    void setOnRecordModeChange(const OnRecordModeChange &cb) { _on_change = std::move(cb); }

private:
    void onSchedulerChange(RecordScheduleItem::Ptr &item);

    void onRecordAlwaysMode();

    void onRecordOnlyMotionMode();

    void onNoRecordMode();

    void onRecordLowResAndMotionMode();

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
    std::unordered_map<int, RecordState> _state_map;
    std::unordered_map<std::string, RecordScheduleItem::Ptr> _schedules;
    OnRecordModeChange _on_change;

    // runtime
    uint64_t _last_switch_record_ms = 0;
    RecordMode _current_mode = RecordMode::NoRecord;
    bool _event_active = false;
};

} // namespace managerkit

#endif // EXTENSION_RECORDPOLICY_H