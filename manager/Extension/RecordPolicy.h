#ifndef EXTENSION_RECORDPOLICY_H
#define EXTENSION_RECORDPOLICY_H

#include <string>
#include "Local/TimeScheduler.h"

namespace managerkit {
    
enum class RecordMode : uint8_t {
    NoRecord = 0,
    RecordOnlyMotion,
    RecordLowResAndMotion,
    RecordAlways,
    RecordModeMax
};

// inline std::string toString(RecordMode mode) {
//     switch (mode) {
//         case RecordMode::NoRecord:             return "NoRecord";
//         case RecordMode::RecordOnlyMotion:     return "RecordOnlyMotion";
//         case RecordMode::RecordLowResAndMotion:return "RecordLowResAndMotion";
//         case RecordMode::RecordAlways:         return "RecordAlways";
//         default:                               return "Unknown";
//     }
// }

// enum class RecordState : uint8_t {
//     Idle = 0,
//     Recording,
// };

// struct RecordScheduleItem {
//     int day = 0; // 0 = Sunday, 1 = Monday, ... 6 = Saturday
//     int hour = 0; // 0-23
//     RecordMode mode = RecordMode::NoRecord;
//     int fps = 0;
//     std::string q;
// };

// class RecordingController {
// public:
//     RecordingController();

//     ~RecordingController();

//     void setupScheduler(const std::string &schedule_str);  
    
//     RecordScheduleItem getRecordScheduledActive(const std::string &time_str);

//     void setMode(RecordMode mode);

//     void onEventStart();

//     void onEventStop();

// private:
//     void evaluate();

//     void applyState(bool should_record);

//     void startRecording();

//     void stopRecording();


// private:
//     std::mutex _mtx;
//     RecordMode _current_mode;
//     RecordState _state;
//     bool _event_active;
//     std::unordered_map<std::string, RecordScheduleItem> _schedules;
// };

// class RecordingPolicyJob : public IScheduledJob {
// public:
//     RecordingPolicyJob();

//     ~RecordingPolicyJob() override;

//     void execute() override;

//     void addRecordingController(std::string key, std::weak_ptr<RecordingController> ctrl) {
//         _map_ctrl.emplace(key, ctrl);
//     }

//     void removeRecordingController(const std::string &key) {
//         _map_ctrl.erase(key);
//     }

// private:
//     std::mutex _mtx_job;
//     std::unordered_map<std::string, std::weak_ptr<RecordingController>> _map_ctrl;
// };

} // namespace managerkit

#endif // EXTENSION_RECORDPOLICY_H