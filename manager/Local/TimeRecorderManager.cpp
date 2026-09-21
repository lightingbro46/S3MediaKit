#include "TimeRecorderManager.h"
#include "TimeFileDirtyTracker.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace TimeRecord {
// How long a timefile writer may sit without a single block written before it is closed again.
// A device kept in the list but no longer recording (moved to another node, or waiting for
// retention to clear its data) otherwise holds its `.s3db` + `.idx` handles for as long as the
// process lives. Reopening costs one append-mode open, so the threshold only has to be well above
// the gap between two blocks of a recording stream. 0 disables closing.
const std::string kTimefileIdleCloseSec = "record.timefileIdleCloseSec";

static toolkit::onceToken token([]() {
    mINI::Instance()[kTimefileIdleCloseSec] = 600;
});
} // namespace TimeRecord

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

size_t TimeRecorderManager::closeIdleWriters() {
    GET_CONFIG(int, idle_close_sec, TimeRecord::kTimefileIdleCloseSec);
    if (idle_close_sec <= 0) {
        return 0;
    }
    std::vector<TimeRecorder::Ptr> recorders;
    {
        // Copy out: closing a file is slow and must not hold the manager lock, which addBlock()
        // takes on the recording path.
        std::lock_guard<std::mutex> lock(_mutex);
        recorders.reserve(_recorders.size());
        for (const auto &pr : _recorders) {
            recorders.push_back(pr.second);
        }
    }
    const auto idle_ms = static_cast<uint64_t>(idle_close_sec) * 1000;
    size_t closed = 0;
    for (const auto &recorder : recorders) {
        closed += recorder->closeIdleWriters(idle_ms);
    }
    if (closed) {
        InfoL << "Closed " << closed << " idle time file writer(s) of " << recorders.size() << " device(s)";
    }
    return closed;
}

} // namespace managerkit
