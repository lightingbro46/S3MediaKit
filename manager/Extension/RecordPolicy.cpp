// #include "RecordingPolicy.h"
// #include <mutex>
// #include <json/json.h>
// #include "Util/util.h"
// #include "Common/StrUtil.h"

// using namespace std;
// using namespace toolkit;

// namespace managerkit {

// static bool readJsonString(const string &json_str, Json::Value &out) {
//     // parse json string to json var
//     Json::CharReaderBuilder builder;
//     builder["collectComments"] = false;
//     Json::Value data;
//     string errs;

//     unique_ptr<Json::CharReader> reader(builder.newCharReader());
//     if (!reader->parse(json_str.c_str(), json_str.c_str() + json_str.size(), &data, &errs)) {
//         WarnL << "Parse json string failed: " << errs;
//         return false;
//     }
//     // get stream information from json var
//     TraceL << "Json data: " << data.toStyledString();
//     out = data;
//     return true;
// }

// static unordered_map<std::string, RecordScheduleItem> parseRecordScheduleStr(const string &str) {
//     unordered_map<std::string, RecordScheduleItem> ret;

//     Json::Value root;
//     if (!readJsonString(str, root)) {
//         WarnL << "Failed to parse record schedule string";
//         return ret;
//     }

//     if (!root.isArray()) {
//         WarnL << "Record schedule json is not an array";
//         return ret;
//     }

//     for (const auto &item : root) {
//         RecordScheduleItem s;
//         string key;
//         if (item.isMember("dh") && item["dh"].isString()) {
//             // format: "d,h", e.g. "0,13" = Sunday 13:00
//             string dh_str = item["dh"].asString();
//             auto tmp = split(dh_str, ",");
//             if (tmp.size() == 2) {
//                 string day_str = tmp[0];
//                 string hour_str = tmp[1];
//                 s.day = stoi(day_str);
//                 s.hour = stoi(hour_str);
//             }
//             key = dh_str;
//         }

//         if (item.isMember("fps") && item["fps"].isInt()) {
//             s.fps = item["fps"].asInt();
//         }

//         if (item.isMember("q") && item["q"].isString()) {
//             s.q = item["q"].asString();
//         }

//         if (item.isMember("ty") && item["ty"].isInt()) {
//             auto ty = item["ty"].asInt();
//             s.mode = static_cast<RecordMode>(ty);
//         }

//         ret.emplace(key, s);
//     }

//     return ret;
// }

// RecordingController::RecordingController() : 
//         _current_mode(RecordMode::NoRecord),
//         _state(RecordState::Idle),
//         _event_active(false) {}

// RecordingController::~RecordingController() {
//     _schedules.clear();
// }

// void RecordingController::setupScheduler(const std::string &schedule_str) {
//     _schedules = parseRecordScheduleStr(schedule_str);
// }

// RecordScheduleItem RecordingController::getRecordScheduledActive(const std::string &time_str) {
//     auto it = _schedules.find(time_str);
//     if (it != _schedules.end()) {
//         return it->second;
//     }
//     // default
//     return RecordScheduleItem{};
// }

// void RecordingController::setMode(RecordMode mode) {
//     std::lock_guard<std::mutex> lock(_mtx);
//     _current_mode = mode;
//     evaluate();
// }

// void RecordingController::onEventStart() {
//     std::lock_guard<std::mutex> lock(_mtx);
//     _event_active = true;
//     evaluate();
// }

// void RecordingController::onEventStop() {
//     std::lock_guard<std::mutex> lock(_mtx);
//     _event_active = false;
//     evaluate();
// }

// void RecordingController::evaluate() {
//     bool should_record = false;

//     switch(_current_mode) {
//         case RecordMode::NoRecord:
//             should_record = false;
//             break;
//         case RecordMode::RecordOnlyMotion:
//             should_record = _event_active;
//             break;
//         case RecordMode::RecordLowResAndMotion:
//             should_record = true; // always record low res
//             break;
//         case RecordMode::RecordAlways:
//             should_record = true;
//             break;
//         default:
//             should_record = false;
//             break;
//     }
//     applyState(should_record);
// }

// void RecordingController::applyState(bool should_record) {
//     if (should_record && _state == RecordState::Idle) {
//         // start recording
//         startRecording();
//         _state = RecordState::Recording;
//     } else if (!should_record && _state == RecordState::Recording) {
//         // stop recording
//         stopRecording();
//         _state = RecordState::Idle;
//     }
// }

// void RecordingController::startRecording() {
//     // Implementation to start recording
// }

// void RecordingController::stopRecording() {
//     // Implementation to stop recording
// }

// ////////////////////////////////////////////////////////////////////////////

// RecordingPolicyJob::RecordingPolicyJob()  {}

// RecordingPolicyJob::~RecordingPolicyJob() {}

// void RecordingPolicyJob::execute() {
//     //todo: lặp qua tất cả các RecordingController đã đăng ký
//     auto strong_ctrl = _weak_ctrl.lock();
//     if (!strong_ctrl) {
//         return;
//     }

//     // Get current time
//     auto week_time = StrTimeUtils::getWeekTime(time(nullptr));
//     string time_str = (StrPrinter << week_time.day_of_week << "," << week_time.hour);
//     auto schedule_item = strong_ctrl->getRecordScheduledActive(time_str);
//     strong_ctrl->setMode(schedule_item.mode);
// }

// } // namespace managerkit
