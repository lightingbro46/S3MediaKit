#include "TimeRecorderManager.h"
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

TimeRecorderManager::TimeRecorderManager() {}

TimeRecorderManager::~TimeRecorderManager() {
    std::lock_guard<std::mutex> lock(_mutex);
    _recorders.clear();
    _sd_recorders.clear();
}

bool TimeRecorderManager::addBlock(const TimeBlock &block) {
    auto device_id = block.app();
    if (device_id.empty()) {
        WarnL << "TimeBlock has empty device_id. Ignored";
        return false;
    }

    auto type = block.is_replay() ? RecorderType::LOCAL_SD : RecorderType::LOCAL;

    TimeRecorder::Ptr recorder = getRecorder(device_id, type);
    auto next_open_time = recorder->getNextOpenTime();
    if (next_open_time > 0 && next_open_time <= block.start_time()) {
        // Create new time file if needed
        recorder = addRecorder(device_id);
    }

    // Input time block
    if (type == RecorderType::LOCAL_SD) return recorder->inputSDBlock(block);
    return recorder->inputBlock(block);
}

TimeRecorder::Ptr TimeRecorderManager::getRecorder(const string &device_id, RecorderType type) {
    CHECK(!device_id.empty());
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::unordered_map<std::string, TimeRecorder::Ptr>* recorders = nullptr;
        switch (type) {
            case RecorderType::LOCAL:       recorders = &_recorders;    break;
            case RecorderType::LOCAL_SD:    recorders = &_sd_recorders; break;
        }
        auto it = recorders->find(device_id);
        if (it != recorders->end())
            return it->second;
    }
    return addRecorder(device_id);
}

TimeRecorder::Ptr TimeRecorderManager::addRecorder(const string &device_id, RecorderType type) {
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
        std::unordered_map<std::string, TimeRecorder::Ptr>* recorders = nullptr;
        switch (type) {
            case RecorderType::LOCAL:       recorders = &_recorders;    break;
            case RecorderType::LOCAL_SD:    recorders = &_sd_recorders; break;
        }
        recorders->emplace(device_id, recorder);
    }
    return recorder;
}

bool TimeRecorderManager::removeRecorder(const string &device_id, RecorderType type) {
    TimeRecorder::Ptr recorder;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::unordered_map<std::string, TimeRecorder::Ptr>* recorders = nullptr;
        switch (type) {
            case RecorderType::LOCAL:       recorders = &_recorders;    break;
            case RecorderType::LOCAL_SD:    recorders = &_sd_recorders; break;
        }
        auto it = recorders->find(device_id);
        if (it == recorders->end()) {
            return false;
        }
        recorder = it->second;
    }
    // todo:  remove files associated with this time recorder
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::unordered_map<std::string, TimeRecorder::Ptr>* recorders = nullptr;
        switch (type) {
            case RecorderType::LOCAL:       recorders = &_recorders;  break;
            case RecorderType::LOCAL_SD:    recorders = &_sd_recorders;     break;
        }
        recorders->erase(device_id);
    }
    TraceL << "Removed TimeRecorder for device: " << device_id;
    return true;
}

} // namespace managerkit