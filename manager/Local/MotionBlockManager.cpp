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
    File::delete_file(block_path);
    DebugL << "Deleted motion block files: " << block_path;
    return true;
}

void MotionBlockManager::removeExpiredMotionBlocks(const string &record_path, uint64_t &threshold) {
    GET_CONFIG(string, app_name, Record::kArchiveName);
    auto archive_path = File::absolutePath(app_name, record_path);
    auto motion_block_path = archive_path + "/motion";
    if (!File::is_dir(motion_block_path))
        return;

    auto star_of_day = StampUtils::getStartOfDay(threshold);

    File::scanDir(motion_block_path, [this, star_of_day](const string &path, bool isDir) {
        if (isDir) return true; // skip directories
        if (!end_with(path, ".mblk")) return true; // skip non-motion block files

        auto folder_path = File::parentDir(path);
        auto filename = findSubString(path.data() + folder_path.size(), nullptr, ".mblk");
        uint64_t stamp = StrTimeUtils::getTsFromDateStr(filename);
        TraceL << "Checking motion block file: " << path << ". Timestamp: " << getTimeStr("%Y-%m-%d %H:%M:%S", stamp) << ". Threshold: " << getTimeStr("%Y-%m-%d %H:%M:%S", star_of_day);
        if (stamp < star_of_day) {
            deleteMotionBlockPair(path);
        }
        return true;
    });

    emitEvent(record_path, true, threshold);

}

void MotionBlockManager::emitEvent(const string &record_path, bool start, uint64_t &threshold) {
    auto parent_path = File::parentDir(record_path);
    auto device_id = findSubString(record_path.data() + parent_path.size(), nullptr, nullptr);
    auto flag = NOTICE_EMIT(BroadcastMotionKeepThresholdArgs, Broadcast::kBroadcastMotionKeepThreshold, device_id, start, threshold);
    if (!flag) {
        // No one is listening to this event
        DebugL << "No listener for BroadcastMotionKeepThreshold, device_id: " << device_id;
    }
}

} // namespace managerkit