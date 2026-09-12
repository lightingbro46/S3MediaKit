#include "MotionBlockManager.h"
#include "Util/util.h"
#include "Util/File.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Common/StrUtil.h"
#include "Common/DeviceSource.h"
#include "Common/macros.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(MotionBlockManager)

MotionBlockManager::MotionBlockManager(const std::string &file_path, int max_hour) : _record_path(file_path), _max_hour(max_hour) {
    if (_record_path.empty()) {
        GET_CONFIG(string, record_path, Protocol::kMP4SavePath);
        GET_CONFIG(string, record_app, Record::kAppName);
        _record_path = File::absolutePath(record_app, record_path);
    }
}

void MotionBlockManager::removeExpiredMotionBlocks(const std::unordered_map<std::string, uint64_t> &block_thresholds) {
    File::scanDir(_record_path, [this, &block_thresholds](const string &path, bool isDir) {
        if (!isDir) return true; // skip files

        auto device_id = findSubString(path.data() + _record_path.size(), "/", nullptr);
        if (device_id.empty()) return true; // skip if device_id is not found

        uint64_t threshold_s = time(nullptr);
        auto it = block_thresholds.find(device_id);
        if (it != block_thresholds.end()) {
            threshold_s = it->second;
        }
        DebugL << "Removing expired motion blocks for device: " << device_id << ". Threshold: " << getTimeStr("%Y-%m-%d %H:%M:%S", threshold_s);
        removeExpiredMotionBlocks(path, threshold_s);
        
        return true;
    });

}

static bool deleteMotionBlockPair(const std::string &block_path) {
    {
        string index_path = block_path;
        replace(index_path, ".mblk", ".idx");
        File::delete_file(index_path);
    }
    File::delete_file(block_path, true);
    DebugL << "Deleted motion block files: " << block_path;
    return true;
}

void MotionBlockManager::removeExpiredMotionBlocks(const string &record_path, uint64_t &threshold) {
    GET_CONFIG(string, app_name, Record::kArchiveName);
    auto archive_path = File::absolutePath(app_name, record_path);
    auto motion_block_path = archive_path + "/motion";
    if (!File::is_dir(motion_block_path)) {
        return;
    }

    auto start_of_day = StampUtils::getStartOfDay(threshold);

    File::scanDir(motion_block_path, [this, start_of_day](const string &path, bool isDir) {
        if (isDir) return true; // skip directories
        if (!end_with(path, ".mblk")) return true; // skip non-motion block files

        auto folder_path = File::parentDir(path);
        auto filename = findSubString(path.data() + folder_path.size(), nullptr, ".mblk");
        uint64_t stamp = StrTimeUtils::getTsFromDateStr(filename);
        TraceL << "Checking motion block file: " << path << ". Timestamp: " << getTimeStr("%Y-%m-%d %H:%M:%S", stamp) << ". Threshold: " << getTimeStr("%Y-%m-%d %H:%M:%S", start_of_day);
        if (stamp < start_of_day) {
            deleteMotionBlockPair(path);
        }
        return true;
    });

    auto parent_path = File::parentDir(record_path);
    auto device_id = findSubString(record_path.data() + parent_path.size(), nullptr, nullptr);
    emitEvent(device_id, true, threshold);
}

void MotionBlockManager::emitEvent(const string &device_id, bool start, uint64_t &threshold) {
    auto flag = NOTICE_EMIT(BroadcastMotionKeepThresholdArgs, Broadcast::kBroadcastMotionKeepThreshold, device_id, start, threshold);
    if (!flag) {
        // No one is listening to this event
        DebugL << "No listener for BroadcastMotionKeepThreshold, device_id: " << device_id;
    }
}

string MotionBlockManager::getMotionBlockPath(const string &device_id, uint64_t day_start) const {
    GET_CONFIG(string, archive_name, Record::kArchiveName);
    auto device_path = File::absolutePath(device_id, _record_path);
    auto archive_path = File::absolutePath(archive_name, device_path);
    auto date = getTimeStr("%Y-%m-%d", static_cast<time_t>(StampUtils::getStartOfDay(day_start)));
    return archive_path + "/motion/" + date + ".mblk";
}

void MotionBlockManager::refreshMotionKeepThreshold(const string &device_id) {
    GET_CONFIG(string, archive_name, Record::kArchiveName);
    auto device_path = File::absolutePath(device_id, _record_path);
    auto motion_path = device_path + "/" + archive_name + "/motion";
    uint64_t earliest = 0;
    if (File::is_dir(motion_path)) {
        File::scanDir(motion_path, [&](const string &path, bool is_dir) {
            if (is_dir || !end_with(path, ".mblk"))
                return true;
            auto parent = File::parentDir(path);
            auto filename = findSubString(path.data() + parent.size(), nullptr, ".mblk");
            auto stamp = StrTimeUtils::getTsFromDateStr(filename);
            if (stamp && (!earliest || stamp < earliest))
                earliest = stamp;
            return true;
        });
    }
    if (!earliest)
        earliest = static_cast<uint64_t>(time(nullptr));

    emitEvent(device_id, true, earliest);
}

void MotionBlockManager::removeMotionBlockDays(const string &device_id, const vector<uint64_t> &day_starts) {
    if (device_id.empty() || day_starts.empty()) {
        WarnL << "Device_id is empty or day_starts is empty, skipping";
        return;
    }
    for (const auto &day_start : day_starts) {
        auto block_path = getMotionBlockPath(device_id, day_start);
        if (File::fileExist(block_path)) {
            deleteMotionBlockPair(block_path);
            TraceL << "Deleted motion block for device=" << device_id << " day=" << getTimeStr("%Y-%m-%d", static_cast<time_t>(day_start));
        } else {
            TraceL << "Motion block file does not exist for device=" << device_id << " day=" << getTimeStr("%Y-%m-%d", static_cast<time_t>(day_start));
        }
    }
    refreshMotionKeepThreshold(device_id);
}

} // namespace managerkit