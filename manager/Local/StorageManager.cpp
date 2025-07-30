#include <ctime>
#include <cmath>
#include <iomanip>
#include "Util/util.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Thread/WorkThreadPool.h"
#include "TimeRecorder.h"
#include "Camera/GenericRtspCamera.h"
#include "Server/GlobalMonitor.h"
#include "StorageManager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(StorageManager)

StorageManager::~StorageManager() {
    _timer.reset();
}

StorageManager::StorageManager(const EventPoller::Ptr &poller) {
    _poller = poller ? std::move(poller) : EventPollerPool::Instance().getPoller();

    cleanupTemporaryFiles();
}

static void cleanupFolder(const string folder) {
    File::scanDir(folder, [](const string &path, bool isDir) {
        File::delete_file(path);
        return true;
    });
}

void StorageManager::cleanupTemporaryFiles() {
    // reset ticker to elapse time
    _ticker.resetTime();

    GET_CONFIG(string, hls_save_path, Protocol::kHlsSavePath)
    cleanupFolder(hls_save_path);
    DebugL << "Cleanup hls save path";

    GET_CONFIG(string, snap_save_path, "api.snapRoot");
    cleanupFolder(snap_save_path);
    DebugL << "Cleanup snap save path";

    GET_CONFIG(string, extract_save_path, "api.extractRoot")
    cleanupFolder(extract_save_path);
    DebugL << "Cleanup extract save path";

    InfoL << "Remove temporary files. Finished. " << formatDuration(_ticker.elapsedTime()) << " elapsed" ;
}

using RecordProfiles = unordered_map<string /*camera_id/stream_id*/, pair<uint64_t /*min_value*/, uint64_t /*max_value*/>>;

using KeepTimeThresholdMap = unordered_map<string /*camera_id/stream_id*/, uint64_t /*keep_time*/>;

static RecordProfiles getRecordProfiles() {
    RecordProfiles profiles;
    time_t current_time = time(nullptr);
    CameraSource::for_each_camera([&](const CameraSource::Ptr &src) {
        auto ptr = dynamic_pointer_cast<GenericRtspCamera>(src);
        if (ptr) {
            auto info = ptr->getCameraInfo();
            auto option = ptr->getCameraOption();
            uint64_t min_value = 0;
            uint64_t max_value = 0;
            if (option.keepArchivedMinForAuto) {
                min_value = current_time;
            } else {
                min_value = current_time - option.keepArchivedMinFor;
            }

            if (option.keepArchivedMaxForAuto) {
                //todo: get first block from any stream proxy
            } else {
                max_value = current_time - option.keepArchivedMaxFor;
            }

            if (!info.primary_url.empty()) {
                string key_primary = (StrPrinter << info.camera_id << "/" << info.primary_id);
                profiles.emplace(key_primary, make_pair(min_value, max_value));
            }
            if (!info.secondary_url.empty()) {
                string key_second = (StrPrinter << info.camera_id << "/" << info.secondary_id);
                profiles.emplace(key_second, make_pair(min_value, max_value));
            }
        }
    });
    return profiles;
}

static size_t recreateTimeFile(KeepTimeThresholdMap &thresholds, size_t space_reclaim) {
    auto record_profiles = getRecordProfiles();

    size_t removed_bytes = 0;
    double keep_percent = 100.0;

    auto keep_block = [&](const TimeBlock &block) { 
        string key = (StrPrinter << block.app() << "/" << block.stream()); 
        if (thresholds.find(key) != thresholds.end()) {
            if (block.start_time() >= thresholds[key]) {
                return true;
            }
        }
        removed_bytes += block.file_size();
        return false;
    };

    while (keep_percent > 0 && removed_bytes < space_reclaim) {
        // estimate threshold with new keep_percent value
        for (const auto &p : record_profiles) {
            auto keep_pair = p.second;
            thresholds[p.first] = (keep_pair.second - keep_pair.first) * keep_percent / 100 + keep_pair.first;
        }
        // TimeRecorder::Instance().recreateTimeMarker(keep_block);
        // decrease keep_percent to estimate removed bytes again in next loop
        if (removed_bytes < space_reclaim) {
            keep_percent += (-5.0);
            TraceL << "Decrease keep percent: " << format_double_2f(keep_percent) << "%";
        }
    }

    DebugL << "Recreate time file. Keep percent: " << format_double_2f(keep_percent) << "%. Removed bytes: " << format_bytes_human_readable(removed_bytes);
    return removed_bytes;
}

static bool findMountPoint(const std::string& path, double &usage_pct, size_t &total_bytes) {
    usage_pct = 0.0;
    total_bytes = 0;
    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    string best_match;
    for (const auto& disk : hdd_usage) {
        string mp = disk.mount_point;
        if (start_with(path, mp)) { // path starts with mp
            if (best_match.empty() || mp.size() > best_match.size()) {
                best_match = mp;
                usage_pct = disk.usage_pct;
                total_bytes = disk.total_bytes;
            }
        }
    }
    TraceL << "Found mountpoint: " << best_match;
    return !best_match.empty();
}

static size_t estimateSpaceToReclaim() {
    size_t space_reclaim = 0;
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    string record_path = File::absolutePath(app_name, mp4_save_path);
    double usage_pct = 0.0;
    size_t total_bytes = 0;

    if (findMountPoint(record_path, usage_pct, total_bytes)) {
        // todo: estimate with read/write speed
        if (usage_pct >= 90.0) {
            space_reclaim = static_cast<size_t>((usage_pct - 85.0)) * total_bytes / 100;
        }
    }
    DebugL << "Estimate space to reclaim: " << format_bytes_human_readable(space_reclaim);

    return space_reclaim;
}

static uint64_t findStartTime(const string &time_path) {
    // time_path ví dụ: "2025-07-16/14-38-34-1"
    std::tm tm = {};
    
    // Tách ngày và giờ
    size_t pos = time_path.find('/');
    if (pos == std::string::npos) return 0; // không hợp lệ

    std::string datePart = time_path.substr(0, pos);       // "2025-07-16"
    std::string timePart = time_path.substr(pos + 1);      // "14-38-34-1"

    // Loại bỏ phần cuối "-1" nếu có
    size_t extraPos = timePart.rfind('-');
    if (extraPos != std::string::npos) {
        timePart = timePart.substr(0, extraPos); // "14-38-34"
    }

    // Ghép thành định dạng "YYYY-MM-DD HH:MM:SS"
    std::string full = datePart + " " + timePart;
    std::replace(full.begin() + 11, full.end(), '-', ':'); // đổi '-' sau giờ thành ':'

    // Parse với std::get_time
    std::istringstream ss(full);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) return 0;

    // Chuyển sang timestamp
    time_t t = mktime(&tm);
    return static_cast<uint64_t>(t);

}

static size_t removeExpiredSegment(const string &stream_path, const string &camera_id, const string  &stream_id, uint64_t time_threshold) {
    size_t removed_bytes = 0;
    // find segment which create time is under time threshold
    unordered_map<string, size_t> remove_files;
    File::scanDir(stream_path, [&remove_files, time_threshold, stream_path](const string path, bool isDir) {
        if (!isDir && end_with(path, ".mp4")) {
            string relative_path = findSubString(path.data() + stream_path.size(), "/", ".mp4");
            auto start_time = findStartTime(relative_path);
            if (time_threshold < start_time) {
                return false;
            }
            size_t file_size = File::fileSize(path);
            remove_files.emplace(path, file_size);
        }
        return true;
    }, true);
    // delete selected files and sum file size that removed
    for (const auto &it : remove_files) {
        File::delete_file(it.first, true);
        removed_bytes += it.second;
    }
    return removed_bytes;
}

void StorageManager::enforceStoragePolicy() {
    size_t space_reclaim = estimateSpaceToReclaim();

    KeepTimeThresholdMap thresholdMap;
    size_t removed_bytes = recreateTimeFile(thresholdMap, space_reclaim);

    // asynchronous delete expired segment by scanning folder and removing file which start time over threshold
    weak_ptr<StorageManager> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self, thresholdMap]() {
        // Switch back to your own thread
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        // todo: Execution time exceeds 30 minutes
        GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath)
        GET_CONFIG(string, appName, Record::kAppName)
        auto record_path = File::absolutePath(appName, mp4_save_path);
        File::scanDir(record_path, [&](const string path, bool isDir) {
            if (isDir) {
                DebugL << path;
                auto sub_path = findSubString(path.data() + record_path.size(), "/", nullptr);
                auto tuples = split(sub_path, "/");
                if (tuples.size() == 2) {
                    string camera_id = tuples[0];
                    string stream_id = tuples[1];
                    string key = (StrPrinter << camera_id << "/" << stream_id);
                    uint64_t threshold = time(nullptr);
                    auto it = thresholdMap.find(key);
                    if (it != thresholdMap.end()) {
                        threshold = it->second;
                    }
                    removeExpiredSegment(path, camera_id, stream_id, threshold);
                    TraceL << "Remove expired file: " << path << ". Threshold: " << getTimeStr("%Y-%m-%d %H:%M:%S", threshold);
                }
            }
            return true;
        }, true);
    });
}

void StorageManager::start() {
    if (_timer) {
        WarnL << "Storage manager has been running. Ignore";
        return;
    }

    // weak_ptr<StorageManager> weak_self = shared_from_this();
    // _timer = std::make_shared<Timer>(
    //     1800.0f,
    //     [weak_self]() {
    //         auto strong_self = weak_self.lock();
    //         if (!strong_self) {
    //             return false;
    //         }
    //         strong_self->enforceStoragePolicy();
    //         return true;
    //     },
    //     _poller);
}

void StorageManager::getMainStorageUsage(double &usage_pct, size_t &total_bytes) {
    usage_pct = 0.0;
    total_bytes = 0;
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    string record_path = File::absolutePath(app_name, mp4_save_path);

    if (!findMountPoint(record_path, usage_pct, total_bytes)) {
        WarnL << "Not found main storage: " << record_path;
    }
}

void StorageManager::getBackUpStorageUsage(double &usage_pct, size_t &total_bytes) {
    usage_pct = 0.0;
    total_bytes = 0;
    // todo:
}

} // namespace managerkit
