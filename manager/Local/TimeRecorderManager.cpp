#include "TimeRecorderManager.h"
#include "TimeFileDirtyTracker.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

static onceToken token(
    []() { 
        GOOGLE_PROTOBUF_VERIFY_VERSION;
    },
    []() { google::protobuf::ShutdownProtobufLibrary(); }
);

INSTANCE_IMP(TimeRecorderManager)

TimeRecorderManager::TimeRecorderManager() {
    // Restore the persisted dirty-file set so a restart does not lose track of files
    // that still need to be rebuilt.
    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, appName, Record::kAppName);
    auto record_path = File::absolutePath(appName, recordPath);
    TimeFileDirtyTracker::Instance().loadFromDisk(record_path);
}

TimeRecorderManager::~TimeRecorderManager() {
    std::lock_guard<std::mutex> lock(_mutex);
    _recorders.clear();
}

bool TimeRecorderManager::addBlock(const TimeBlock &block) {
    auto device_id = block.app();
    if (device_id.empty()) {
        WarnL << "TimeBlock has empty device_id. Ignored";
        return false;
    }

    TimeRecorder::Ptr recorder = getRecorder(device_id);

    // Input time block
    return recorder->inputBlock(block);
}

TimeRecorder::Ptr TimeRecorderManager::getRecorder(const string &device_id) {
    CHECK(!device_id.empty());
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _recorders.find(device_id);
        if (it != _recorders.end())
            return it->second;
    }
    return addRecorder(device_id);
}

TimeRecorder::Ptr TimeRecorderManager::addRecorder(const string &device_id) {
    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, appName, Record::kAppName);
    auto record_path = File::absolutePath(appName, recordPath);
    string full_path = record_path + "/" + device_id;

    if (!File::is_dir(full_path)) {
        throw std::runtime_error("Src path is not a directory: " + full_path);
    }

    auto recorder = make_shared<TimeRecorder>(full_path);
    TraceL << "Created TimeRecorder for device: " << device_id;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_recorders.find(device_id) != _recorders.end()) {
            _recorders.erase(device_id);
        }
        _recorders.emplace(device_id, recorder);
    }
    return recorder;
}

bool TimeRecorderManager::removeRecorder(const string &device_id) {
    TimeRecorder::Ptr recorder;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _recorders.find(device_id);
        if (it == _recorders.end()) {
            return false;
        }
        recorder = it->second;
    }
    // todo:  remove files associated with this time recorder
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _recorders.erase(device_id);
    }
    TraceL << "Removed TimeRecorder for device: " << device_id;
    return true;
}

} // namespace managerkit
