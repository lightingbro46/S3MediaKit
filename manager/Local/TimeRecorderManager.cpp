#include "TimeRecorderManager.h"
#include "TimeFileDirtyTracker.h"
#include "TimeRebuilder.h"
#include "Common/config.h"
#include "Common/BlockingExecutors.h"
#include "Poller/EventPoller.h"

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

// Rewrite in order the time files where gap fill or SD-card import appended a block after newer
// ones; windowed queries (playback, tier ranges) do not see such a block until then.
const std::string kTimefileSortEnabled = "record.timefileSortEnabled";
// Files rewritten per pass; the rest wait for the next tier cycle.
const std::string kTimefileSortMaxFilesPerCycle = "record.timefileSortMaxFilesPerCycle";
// Delay after a gap-fill connection closed, so its last segment's block is written first.
const std::string kTimefileSortDelayAfterGapFillMs = "record.timefileSortDelayAfterGapFillMs";

static toolkit::onceToken token([]() {
    mINI::Instance()[kTimefileIdleCloseSec] = 600;
    mINI::Instance()[kTimefileSortEnabled] = 1;
    mINI::Instance()[kTimefileSortMaxFilesPerCycle] = 50;
    mINI::Instance()[kTimefileSortDelayAfterGapFillMs] = 10000;
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

size_t TimeRecorderManager::sortUnsortedTimeFiles(const string &device_id) {
    GET_CONFIG(bool, enabled, TimeRecord::kTimefileSortEnabled);
    if (!enabled) {
        return 0;
    }
    GET_CONFIG(int, max_files, TimeRecord::kTimefileSortMaxFilesPerCycle);
    const size_t budget = max_files > 0 ? static_cast<size_t>(max_files) : 50;
    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, appName, Record::kAppName);
    const string record_path = File::absolutePath(appName, recordPath);

    // Keyed like TimeRecorder marks them: the device directory without a trailing slash.
    vector<string> device_paths;
    if (device_id.empty()) {
        device_paths = TimeFileDirtyTracker::Instance().devicesWithUnsorted();
    } else {
        device_paths.push_back(record_path + "/" + device_id);
    }

    size_t sorted = 0;
    for (const auto &device_path : device_paths) {
        if (sorted >= budget) {
            break;
        }
        if (!File::is_dir(device_path.data())) {
            continue;
        }
        sorted += MultiTimeRebuilder(device_path).sortTimeFiles(budget - sorted);
    }
    if (sorted) {
        InfoL << "Rewrote " << sorted << " out-of-order time file(s) in start-time order";
    }
    return sorted;
}

void TimeRecorderManager::postSortTimeFiles(const string &device_id) {
    if (device_id.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_sort_pending.insert(device_id).second) {
            return;
        }
    }
    GET_CONFIG(int, delay_ms, TimeRecord::kTimefileSortDelayAfterGapFillMs);
    EventPollerPool::Instance().getPoller()->doDelayTask(std::max(0, delay_ms), [device_id]() {
        // Rewriting files blocks: storage pool, not the poller.
        BlockingExecutors::storage()->async([device_id]() {
            {
                // Cleared before running, so a gap fill closing during the sort queues another.
                std::lock_guard<std::mutex> lock(TimeRecorderManager::Instance()._mutex);
                TimeRecorderManager::Instance()._sort_pending.erase(device_id);
            }
            TimeRecorderManager::Instance().sortUnsortedTimeFiles(device_id);
        }, false);
        return 0;
    });
}

} // namespace managerkit
