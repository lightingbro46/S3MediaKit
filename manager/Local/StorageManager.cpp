#include <ctime>
#include <cmath>
#include <iomanip>
#include "Util/util.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Thread/WorkThreadPool.h"
#include "TimeRecorderManager.h"
#include "TimeRebuilder.h"
#include "Camera/GenericRtspCameraImp.h"
#include "Server/GlobalMonitor.h"
#include "StorageManager.h"
#include "server/Manager.h"
#include "Storage/UserSession.h"
#include "Storage/Bookmark.h"
#include "Common/StrUtil.h"
#include "MotionBlockManager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(StorageManager)

StorageManager::~StorageManager() {
    stop();
}

StorageManager::StorageManager(const EventPoller::Ptr &poller) {
    _poller = poller ? std::move(poller) : EventPollerPool::Instance().getPoller();

    GET_CONFIG(bool, legacy_temp_cleanup_enabled, Storage::kLegacyTempCleanupEnabled);
    if (legacy_temp_cleanup_enabled)
        cleanupTemporaryFiles();
}

static void cleanupFolder(const string folder) {
    File::scanDir(folder, [](const string &path, bool isDir) {
        File::delete_file(path);
        return true;
    });
}

static void cleanCrashFiles() {
    vector<string> crash_file_paths;
    auto root_path = File::absolutePath("./", "");
    File::scanDir(root_path, [&](const string &path, bool isDir) {
        if (isDir) return true;
        auto filename = findSubString(path.data() + root_path.size(), nullptr, nullptr);
        if (start_with(filename, "crash.") || start_with(filename, "core.")) {
            crash_file_paths.push_back(path);
        }
        return true;
    });

    for (const auto &path : crash_file_paths) {
        File::delete_file(path);
    }
}

void StorageManager::cleanupTemporaryFiles() {
    if ((uint64_t)time(nullptr) < _next_cleanup_time) {
        return;
    }

    // reset ticker to elapse time
    _ticker.resetTime();

    if (_next_cleanup_time == 0) {
        // only cleanup hls save path when the manager is started for the first time, 
        // to avoid deleting files created by other instances in cluster deployment
        GET_CONFIG(string, hls_save_path, Protocol::kHlsSavePath)
        cleanupFolder(hls_save_path);
        DebugL << "Cleanup hls save path";
    }
    
    GET_CONFIG(string, snap_save_path, "api.snapRoot");
    cleanupFolder(snap_save_path);
    DebugL << "Cleanup snap save path";

    GET_CONFIG(string, extract_save_path, "api.extractRoot")
    cleanupFolder(extract_save_path);
    DebugL << "Cleanup extract save path";

    GET_CONFIG(string, ffmpeg_log, "ffmpeg.log")
    auto parent_path = File::parentDir(ffmpeg_log);
    cleanupFolder(parent_path);
    DebugL << "Cleanup FFmpeg log files";

    cleanCrashFiles();
    DebugL << "Cleanup crash files";

    InfoL << "Remove temporary files. Finished. " << format_duration_verbose(_ticker.elapsedTime()) << " elapsed" ;

    _next_cleanup_time = StampUtils::getStartOfDay(time(nullptr)) + 86400;
    InfoL << "Next temporary files cleanup time: " << getTimeStr("%Y-%m-%d %H:%M:%S", _next_cleanup_time);
}

using KeepTimeMap = TimeRebuilder::KeepTimeMap;

static size_t recreateTimeFile(const KeepTimeMap &map) {
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath)
    GET_CONFIG(string, appName, Record::kAppName)
    auto record_path = File::absolutePath(appName, mp4_save_path);

    std::unordered_map<string, string> device_record_map;

    File::scanDir(record_path, [&](const string path, bool isDir) {
        if (isDir) {
            auto device_id = findSubString(path.data() + record_path.size(), "/", nullptr);
            device_record_map.emplace(device_id, path);
        }
        return true;
    });

    size_t removed_timefile_bytes = 0;
    for (const auto &it : device_record_map) {
        auto &device_id = it.first;
        auto &src_path = it.second;
        {
            auto rebuilder = std::make_shared<MultiTimeRebuilder>(src_path);
            size_t removed_bytes = rebuilder->rebuildTimeLine(map);
            removed_timefile_bytes += removed_bytes;
            DebugL << "Recreated time file for device: " << device_id << ". Removed bytes: " << format_bytes_human_readable(removed_bytes);
        }
    }
    DebugL << "Recreated all time files. Removed total bytes: " << format_bytes_human_readable(removed_timefile_bytes);
    return removed_timefile_bytes;
}

static size_t recreateCameraTimeFile(const string &device_id, uint64_t threshold) {
    if (device_id.empty())
        return 0;

    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath)
    GET_CONFIG(string, appName, Record::kAppName)
    auto record_path = File::absolutePath(appName, mp4_save_path);
    auto device_path = record_path + "/" + device_id;
    if (!File::is_dir(device_path)) {
        WarnL << "Skip rebuild time file, camera path not found: " << device_path;
        return 0;
    }

    TimeRebuilder::KeepTimeMap keep_time_map;
    File::scanDir(device_path, [&](const string &stream_path, bool isDir) {
        if (!isDir)
            return true;
        auto slash = stream_path.find_last_of('/');
        if (slash == string::npos)
            return true;
        string stream_id = stream_path.substr(slash + 1);
        if (stream_id.empty())
            return true;
        keep_time_map[(StrPrinter << device_id << "/" << stream_id)] = threshold;
        return true;
    }, false, false);

    if (keep_time_map.empty()) {
        WarnL << "Skip rebuild time file, no stream folder under: " << device_path;
        return 0;
    }

    auto rebuilder = std::make_shared<MultiTimeRebuilder>(device_path);
    auto removed_bytes = rebuilder->rebuildTimeLine(keep_time_map);
    InfoL << "Rebuilt time file for device " << device_id
          << " threshold=" << getTimeStr("%Y-%m-%d %H:%M:%S", threshold)
          << " removed=" << format_bytes_human_readable(removed_bytes);
    return removed_bytes;
}

static size_t estimateSpaceToReclaim() {
    size_t space_reclaim = 0;
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    string record_path = File::absolutePath(app_name, mp4_save_path);
    float usage_pct = 0.0f;
    size_t used_bytes = 0;
    size_t total_bytes = 0;

    if (GlobalMonitor::Instance().findMountPointUsage(record_path, usage_pct, used_bytes, total_bytes)) {
        // todo: estimate with read/write speed
        GET_CONFIG(int, limit_percent_usage, Storage::kLimitPercentUsage);
        auto _limit_percent_usage = std::min(99, std::max(0, limit_percent_usage));
        GET_CONFIG(int, remove_percent_extra, Storage::kRemovePercentExtra);
        auto _remove_percent_extra = std::min(99, std::max(0, remove_percent_extra));
        if (_limit_percent_usage <= _remove_percent_extra) {
            // avoid aggressive deletion when limit percentage is set too low, e.g. 0, or remove extra percentage is set too high, e.g. 100
            _remove_percent_extra = 5;
        }
        if (usage_pct >= static_cast<float>(_limit_percent_usage)) {
            space_reclaim = static_cast<size_t>((usage_pct - static_cast<float>(_limit_percent_usage) + static_cast<float>(_remove_percent_extra))) * total_bytes / 100;
        }
    }
    DebugL << "Estimate space to reclaim: " << format_bytes_human_readable(space_reclaim);

    return space_reclaim;
}

using RecordProfiles = unordered_map<string /*camera_id/stream_id*/, pair<uint64_t /*min_value*/, uint64_t /*max_value*/>>;

static RecordProfiles getRecordProfiles() {
    RecordProfiles profiles;
    time_t current_time = time(nullptr);
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
        auto weak_listener = src->getListener();
        if (auto strong_listener = weak_listener.lock()) {
            auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
            if (impl) {
                auto camera = dynamic_pointer_cast<GenericRtspCamera>(src);
                auto option = impl->getCameraOption();
                uint64_t min_value = 0;
                uint64_t max_value = 0;
                if (option.keepArchivedMinForAuto) {
                    min_value = current_time;
                } else {
                    min_value = current_time - option.keepArchivedMinFor;
                }

                if (option.keepArchivedMaxForAuto) {
                    auto stats_imp = impl->getCameraStatisticImp();
                    if (stats_imp) {
                        // get oldest block from both stream proxy
                        auto storage_stats = stats_imp->getParams().storage_map;
                        uint64_t first_block_time = 0;
                        for (const auto &it : storage_stats) {
                            if (first_block_time == 0 || (it.second.archiveStartTime != 0 && it.second.archiveStartTime < first_block_time)) {
                                first_block_time = it.second.archiveStartTime;
                            }
                        }
                        if (first_block_time != 0) {
                            max_value = first_block_time;
                        }
                    }
                    if (max_value == 0) {
                        // no recorded statistic data, use default 6 months
                        uint64_t max_keep_archived_max_for =  6 * 30 * 24 * 3600; // default 6 months
                        max_value = current_time - max_keep_archived_max_for;
                    }
                } else {
                    max_value = current_time - option.keepArchivedMaxFor;
                }

                if (camera->hasStreamTuple(PrimaryStream)) {
                    auto tuple = camera->getStreamTuple(PrimaryStream);
                    string key_primary = (StrPrinter << tuple.device_id << "/" << tuple.stream_id);
                    profiles.emplace(key_primary, make_pair(min_value, max_value));
                }
                if (camera->hasStreamTuple(SecondaryStream)) {
                    auto tuple = camera->getStreamTuple(SecondaryStream);
                    string key_second = (StrPrinter << tuple.device_id << "/" << tuple.stream_id);
                    profiles.emplace(key_second, make_pair(min_value, max_value));
                }
            }
        }
    });
    return profiles;
}

static size_t removeExpiredSegment(const string &stream_path, uint64_t time_threshold) {
    size_t removed_bytes = 0;
    // find segment which create time is under time threshold
    unordered_map<string, size_t> remove_files;
    File::scanDir(stream_path, [&remove_files, time_threshold, stream_path](const string date_path, bool isDir) {
        if (isDir) {
            string date_string = findSubString(date_path.data() + stream_path.size(),"/", nullptr);
            auto date_time = StrTimeUtils::getTsFromDateStr(date_string);
            if (time_threshold <= date_time) {
                return true;
            }
            File::scanDir(date_path, [&remove_files, time_threshold, stream_path](const string path, bool isDir) {
                if (!isDir && end_with(path, ".mp4")) {
                    string relative_path = findSubString(path.data() + stream_path.size(), "/", ".mp4");
                    auto start_time = StrTimeUtils::getTsFromDateTimeStr(relative_path);
                    auto start_time_alt = StrTimeUtils::getTsFromDateTimeStr2(relative_path);
                    if (time_threshold <= start_time || time_threshold <= start_time_alt) {
                        return true;
                    }
                    size_t file_size = File::fileSize(path);
                    remove_files.emplace(path, file_size);
                }
                return true;
            }, false, true);
        }
        return true;
    });
    // delete selected files and sum file size that removed
    for (const auto &it : remove_files) {
        File::delete_file(it.first, true);
        removed_bytes += it.second;
    }
    return removed_bytes;
}

static size_t removeExpiredSegment(const KeepTimeMap &keep_time_map) {
    // todo: Execution time exceeds period time, default 10 minutes
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath)
    GET_CONFIG(string, appName, Record::kAppName)
    GET_CONFIG(string, archive_name, Record::kArchiveName)
    GET_CONFIG(uint32_t, s_max_second, Protocol::kMP4MaxSecond)
    auto record_path = File::absolutePath(appName, mp4_save_path);
    unordered_map<string, uint64_t> path_threshold;
    Ticker ticket;
    File::scanDir(record_path, [&](const string path, bool isDir) {
        if (isDir) {
            auto sub_path = findSubString(path.data() + record_path.size(), "/", nullptr);
            auto tuples = split(sub_path, "/");
            if (tuples.size() == 2) {
                string camera_id = tuples[0];
                string stream_id = tuples[1];
                if (stream_id == archive_name) {
                    return true;
                }
                string key = (StrPrinter << camera_id << "/" << stream_id);
                uint64_t threshold = time(nullptr) - s_max_second;
                auto it = keep_time_map.find(key);
                if (it != keep_time_map.end()) {
                    threshold = it->second;
                    DebugL << "Stream " << key << " threshold: " << threshold;
                } else {
                    DebugL << "Stream " << key << " use current time for threshold: " << threshold;
                }
                path_threshold.emplace(path, threshold);
            }
        }
        return true;
    }, true);
    size_t removed_volume_bytes = 0;
    for (const auto &it : path_threshold) {
        auto bytes = removeExpiredSegment(it.first, it.second);
        DebugL << "Remove expired file: " << it.first << ". Threshold: " << (it.second ? getTimeStr("%Y-%m-%d %H:%M:%S", it.second) : 0) 
                << ". Removed bytes: " << format_bytes_human_readable(bytes) << ". Elapsed: " << format_duration_verbose(ticket.elapsedTime());
        removed_volume_bytes += bytes;
    }
    return removed_volume_bytes;
}

static KeepTimeMap getKeepTimeMapByDevice(const KeepTimeMap &path_map) {
    KeepTimeMap camera_map;
    for (const auto &it : path_map) {
        auto info = split(it.first, "/");
        auto &ret = camera_map[info[0]];
        if (!ret || ret > it.second) {
            ret = it.second;
        }
    }
    return camera_map;
}

static void removeExpiredBookmark(const KeepTimeMap &keep_time_map) {
    auto device_time_map = getKeepTimeMapByDevice(keep_time_map);
    auto imp = std::make_shared<BookmarkStatsImp>();
    auto ret = imp->findAll();
    if (ret.size() > 0) {
        auto imp = std::make_shared<BookmarkImp>();
        for (const auto &it : ret) {
            string device_id = it.device_id;
            auto time_threshold = time(nullptr);
            if (device_time_map.find(device_id) != device_time_map.end()) {
                time_threshold = device_time_map[device_id];
            }
            auto count = imp->removeByTimeRange(0, time_threshold, device_id);
            DebugL << "Remove expired bookmark: " << device_id << ". Threshold: " << getTimeStr("%Y-%m-%d %H:%M:%S", time_threshold) << ". Removed count: " << count;
        }
    }
    InfoL << "Remove expired bookmark. Finished";
}

static void removeExpiredMotionBlock(const KeepTimeMap &keep_time_map) {
    auto device_time_map = getKeepTimeMapByDevice(keep_time_map);
    MotionBlockManager::Instance().removeExpiredMotionBlocks(device_time_map);
    InfoL << "Remove expired motion block. Finished";
}

void StorageManager::enforceStoragePolicy() {
    // asynchronous delete expired segment by scanning folder and removing file which start time over threshold
    weak_ptr<StorageManager> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self]() {
        // Switch back to your own thread
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }

        // Reset a timer to track the operation time of a cycle
        strong_self->_ticker.resetTime();

        // estimate removed space need to reclaim
        size_t space_reclaim = estimateSpaceToReclaim();

        KeepTimeMap keep_time_map;
        auto record_profiles = getRecordProfiles();
        int keep_percent = 100;
        size_t removed_bytes = 0;

        while (keep_percent > 0 && (space_reclaim == 0 || removed_bytes < space_reclaim)) {
            // step 1: estimate keep time map with new keep_percent value
            for (const auto &p : record_profiles) {
                auto keep_pair = p.second;
                keep_time_map[p.first] = static_cast<uint64_t>(keep_pair.first - (keep_pair.first - keep_pair.second) * keep_percent / 100);
            }

            // step 2: remove expired segment and get keep time map
            auto removed_volume_bytes = removeExpiredSegment(keep_time_map);
            removed_bytes += removed_volume_bytes;

            // step 3: decrease keep_percent to estimate removed bytes again in next loop if removed_bytes is not enough
            if (removed_bytes < space_reclaim) {
                // todo: auto select keep_percent by read/write speed
                keep_percent += (-5);
                TraceL << "Decrease keep percent by 5% => remain " << keep_percent << "%";
            }

            // only enforce storage policy once if space_reclaim equal 0 byte
            if (space_reclaim == 0) {
                break;
            }
        }

        if (keep_percent == 0 && removed_bytes < space_reclaim) {
            WarnL << "Cannot reclaim enough space: " << format_bytes_human_readable(removed_bytes)
                  << " , expect: " << format_bytes_human_readable(space_reclaim);
        }

        // recreate time file according to keep time map
        recreateTimeFile(keep_time_map);
        // remove expired motion block associated with media segment
        removeExpiredMotionBlock(keep_time_map);

        InfoL << "Finished enforcing storage policy: " << format_bytes_human_readable(removed_bytes) << ". Elapsed: " << format_duration_verbose(strong_self->_ticker.elapsedTime());

        // remove expired items associated with media segment
        removeExpiredBookmark(keep_time_map);
    });
}

void StorageManager::start() {
    if (_timer) {
        WarnL << "Storage manager has been running. Ignore";
        return;
    }

    weak_ptr<StorageManager> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(
        300.0f,
        [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            GET_CONFIG(bool, legacy_record_cleanup_enabled, Storage::kLegacyRecordCleanupEnabled);
            GET_CONFIG(bool, legacy_user_session_cleanup_enabled, Storage::kLegacyUserSessionCleanupEnabled);
            GET_CONFIG(bool, legacy_temp_cleanup_enabled, Storage::kLegacyTempCleanupEnabled);

            if (legacy_record_cleanup_enabled)
                strong_self->enforceStoragePolicy();
            if (legacy_user_session_cleanup_enabled)
                strong_self->removeExpiredUserSession();
            if (legacy_temp_cleanup_enabled)
                strong_self->cleanupTemporaryFiles();
            return true;
        },
        _poller);
}

void StorageManager::stop() {
    _timer.reset();
}

void StorageManager::rebuildTimeFile(const std::string &camera_id, uint64_t threshold) {
    GET_CONFIG(bool, legacy_timefile_rebuild_enabled, Storage::kLegacyTimefileRebuildEnabled);
    if (!legacy_timefile_rebuild_enabled)
        return;
    recreateCameraTimeFile(camera_id, threshold);
}

string StorageManager::getMainStorageMountPoint() {
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    string record_path = File::absolutePath(app_name, mp4_save_path);
    return GlobalMonitor::Instance().findMountPoint(record_path);
}

void StorageManager::getMainStorageUsage(size_t &used_bytes, size_t &total_bytes) {
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    string record_path = File::absolutePath(app_name, mp4_save_path);
    float usage_pct = 0.0f;
    if (!GlobalMonitor::Instance().findMountPointUsage(record_path, usage_pct, used_bytes, total_bytes)) {
        WarnL << "Not found main storage: " << record_path;
    }
}

void StorageManager::getBackUpStorageUsage(size_t &used_bytes, size_t &total_bytes) {
    used_bytes = 0;
    total_bytes = 0;
    // todo:
}

void StorageManager::removeExpiredUserSession() {
    GET_CONFIG(int, sessionExpiryDays, Manager::kSessionExpiryDays);
    int64_t time_threshold = std::time(nullptr) - (sessionExpiryDays * 24 * 3600);
    DebugL << " Remove expire user session before: " << getTimeStr("%Y-%m-%d %H:%M:%S", time_threshold);
    auto imp = make_shared<UserSessionImp>();
    if (imp->removeByStamp(time_threshold)) {
        InfoL<< "Removed expire user session success";
    } else {
        WarnL << "Remove expire user session failed";
    };
}

Json::Value StorageManager::makeSystemStorageJson() {
    Json::Value val = Json::arrayValue;
    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    auto main_mount_point = getMainStorageMountPoint();
    for (const auto &d : hdd_usage) {
        Json::Value disk = d.toJson();
        disk["isMainStorage"] = main_mount_point == d.mount_point;
        disk["enableConfigure"] = false;
        val.append(disk);
    }
    return val;
}

static void* s_tag;

static onceToken g_token(
[]() {
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastRebuildTimeFile, [](BroadcastRebuildTimeFileArgs) {
        WorkThreadPool::Instance().getPoller()->async([device_id, threshold]() {
            StorageManager::Instance().rebuildTimeFile(device_id, threshold);
        });
    });
}, 
[]() {
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastRebuildTimeFile);
});

} // namespace managerkit