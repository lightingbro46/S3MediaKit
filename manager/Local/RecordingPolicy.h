// #ifndef LOCAL_RECORDSTRATEGY_H
// #define LOCAL_RECORDSTRATEGY_H

// #include <string>
// #include "Local/TimeScheduler.h"

// namespace managerkit {
    
// enum class RecordMode : uint8_t {
//     NoRecord = 0,
//     RecordOnlyMotion,
//     RecordLowResAndMotion,
//     RecordAlways,
// };

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

// // recordingSchedule
//         // if (rc.isMember("recordingSchedule") && rc["recordingSchedule"].isArray()) {
//         //     option.recordingSchedules.clear();

//         //     for (const auto &item : rc["recordingSchedule"]) {

//         //         if (!item.isObject())
//         //             continue;

//         //         RecordingSchedule s;

//         //         if (item.isMember("dh") && item["dh"].isString())
//         //             s.dh = item["dh"].asString();

//         //         if (item.isMember("fps") && item["fps"].isInt())
//         //             s.fps = item["fps"].asInt();

//         //         if (item.isMember("q") && item["q"].isString())
//         //             s.q = item["q"].asString();

//         //         if (item.isMember("ty") && item["ty"].isInt())
//         //             s.ty = item["ty"].asInt();

//         //         option.recordingSchedules.push_back(s);
//         //     }
//         // }

// class RecordScheduleHelper {
// public:
//     static std::string toString(RecordMode mode) {
//         switch (mode) {
//             case RecordMode::NoRecord:             return "NoRecord";
//             case RecordMode::RecordOnlyMotion:     return "RecordOnlyMotion";
//             case RecordMode::RecordLowResAndMotion:return "RecordLowResAndMotion";
//             case RecordMode::RecordAlways:         return "RecordAlways";
//             default:                               return "Unknown";
//         }
//     }

//     // static RecordMode fromTy(int mode) {
//     //     if (mode < count()) {
//     //         return static_cast<RecordMode>(mode);
//     //     }
//     //     return RecordMode::NoRecord; // default fallback
//     // }

//     // static RecordMode resolveMode(const RecordingPolicy & policy, WeakTime &time) {
//     //     auto ret = 
//     // }
// };

// class RecordingController {
// public:
//     RecordingController();

//     ~RecordingController();

//     void setMode(RecordMode mode);

//     void onEventStart();

//     void onEventStop();

// private:
//     void evaluate() {
//         bool should_record = false;

//         switch(_current_mode) {
//             case RecordMode::NoRecord:
//                 should_record = false;
//                 break;
//             case RecordMode::RecordOnlyMotion:
//                 should_record = _event_active;
//                 break;
//             case RecordMode::RecordLowResAndMotion:
//                 should_record = true; // always record low res
//                 break;
//             case RecordMode::RecordAlways:
//                 should_record = true;
//                 break;
//             default:
//                 should_record = false;
//                 break;
//         }
//         applyState(should_record);
//     }

//     void applyState(bool should_record) {
//         if (should_record && _state == RecordState::Idle) {
//             // start recording
//             startRecording();
//             _state = RecordState::Recording;
//         } else if (!should_record && _state == RecordState::Recording) {
//             // stop recording
//             stopRecording();
//             _state = RecordState::Idle;
//         }
//     }

//     void startRecording();

//     void stopRecording();

// private:
//     std::mutex _mtx;
//     RecordMode _current_mode;
//     RecordState _state;
//     bool _event_active;
// };

// class RecordingPolicyJob : public IScheduledJob {
// public:
//     RecordingPolicyJob(RecordingController& ctrl);

//     ~RecordingPolicyJob() override;

//     void execute() override;

//     void setupScheduler(const std::string &schedule_str);   
    
//     void stopScheduler();

//     RecordScheduleItem getRecordScheduledActive(uint64_t ts);

// private:
//     std::weak_ptr<RecordingController> _ctrl;
//     std::vector<RecordScheduleItem> _schedules;
// };

// } // namespace managerkit

// #endif // LOCAL_RECORDSTRATEGY_H