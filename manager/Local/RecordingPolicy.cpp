// #include "RecordingPolicy.h"
// #include <mutex>

// using namespace std;

// namespace managerkit {

// static void parseRecordingScheduleStr(const string &input, vector<RecordScheduleItem> &output) {
    
// }

// RecordingController::RecordingController() : 
//         _current_mode(RecordMode::NoRecord),
//         _state(RecordState::Idle),
//         _event_active(false) {}

// RecordingController::~RecordingController() {

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

// } // namespace managerkit
