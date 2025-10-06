#include <ctime>
#include <cmath>
#include <iomanip>
#include "Util/util.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Thread/WorkThreadPool.h"
#include "TimeRecorder.h"
#include "Common/DeviceSource.h"
#include "Server/GlobalMonitor.h"
#include "StorageManager.h"
#include "server/Manager.h"
#include "Storage/UserSession.h"
#include "Storage/Bookmark.h"

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

using KeepTimeMap = TimeRebuilder::KeepTimeMap;

static size_t recreateTimeFile(KeepTimeMap &map, size_t space_reclaim) {
    try {
        auto self = TimeRecorder::Instance().shared_from_this();
        auto rebuilder = std::make_shared<TimeRebuilder>(self);
        return rebuilder->rebuildTimeLine(map, space_reclaim);
    } catch (exception &ex) {
        WarnL << ex.what();
        return 0;
    }
}

static bool findMountPoint(const std::string& path, double &usage_pct, size_t &used_bytes, size_t &total_bytes) {
    usage_pct = 0.0;
    used_bytes = 0;
    total_bytes = 0;
    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    string best_match;
    for (const auto& disk : hdd_usage) {
        string mp = disk.mount_point;
        if (start_with(path, mp)) { // path starts with mp
            if (best_match.empty() || mp.size() > best_match.size()) {
                best_match = mp;
                usage_pct = disk.usage_pct;
                used_bytes = disk.used_bytes;
                total_bytes = disk.total_bytes;
            }
        }
    }
    TraceL << "Found mountpoint: " << best_match;
    return !best_match.empty();
}

static string findMountPoint(const string& path) {
    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    string best_match;
    for (const auto& disk : hdd_usage) {
        string mp = disk.mount_point;
        if (start_with(path, mp)) { // path starts with mp
            if (best_match.empty() || mp.size() > best_match.size()) {
                best_match = mp;
            }
        }
    }
    TraceL << "Found mountpoint: " << best_match;
    return best_match;
}


static size_t estimateSpaceToReclaim() {
    size_t space_reclaim = 0;
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    string record_path = File::absolutePath(app_name, mp4_save_path);
    double usage_pct = 0.0;
    size_t used_bytes = 0.0;
    size_t total_bytes = 0;

    if (findMountPoint(record_path, usage_pct, used_bytes, total_bytes)) {
        // todo: estimate with read/write speed
        if (usage_pct >= 90.0) {
            space_reclaim = static_cast<size_t>((usage_pct - 85.0)) * total_bytes / 100;
        }
    }
    DebugL << "Estimate space to reclaim: " << format_bytes_human_readable(space_reclaim);

    return space_reclaim;
}

static uint64_t findTimestampPath(const string &time_path) {
    // Hỗ trợ các định dạng:
    // 1. "YYYY-MM-DD/HH-MM-SS[-anything]" (định dạng cũ, ví dụ: 2025-07-16/14-38-34-1)
    // 2. "YYYY-MM-DD" (mới: trả về mốc 00:00:00 local time của ngày đó)
    // 3. (mở rộng nhẹ) "YYYY-MM-DD HH:MM:SS" nếu xuất hiện (dùng dấu cách)

    if (time_path.empty()) {
        return 0;
    }

    auto isDate = [](const std::string &s) -> bool {
        // YYYY-MM-DD
        if (s.size() != 10) return false;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i == 4 || i == 7) {
                if (s[i] != '-') return false;
            } else if (!isdigit(static_cast<unsigned char>(s[i]))) {
                return false;
            }
        }
        return true;
    };

    std::tm tm = {};
    tm.tm_isdst = -1; // let mktime determine DST

    // Trường hợp chỉ có ngày: "YYYY-MM-DD"
    if (isDate(time_path)) {
        std::istringstream ds(time_path + " 00:00:00");
        ds >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ds.fail()) return 0;
        time_t t = mktime(&tm);
        return t > 0 ? static_cast<uint64_t>(t) : 0;
    }

    // Nếu chứa dấu cách và có vẻ là dạng "YYYY-MM-DD HH:MM:SS"
    if (time_path.size() >= 19 && time_path[10] == ' ') {
        std::istringstream fs(time_path);
        fs >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (fs.fail()) return 0;
        time_t t = mktime(&tm);
        return t > 0 ? static_cast<uint64_t>(t) : 0;
    }

    // Định dạng cũ: "YYYY-MM-DD/HH-MM-SS(-extra)"
    size_t pos = time_path.find('/');
    if (pos == std::string::npos) {
        // Không phù hợp định dạng nào
        return 0;
    }

    std::string datePart = time_path.substr(0, pos);       // YYYY-MM-DD
    std::string timePart = time_path.substr(pos + 1);      // HH-MM-SS(-extra?)

    if (!isDate(datePart)) {
        return 0;
    }

    // Loại bỏ phần đầu '.' nếu có
    if (start_with(timePart, ".")) {
        timePart.erase(0, 1);
    }

    // Cắt bỏ phần hậu tố không thuộc HH-MM-SS (ví dụ '-1')
    // Chiến lược: lấy đúng 3 nhóm đầu tiên ngăn cách bởi '-'
    {
        auto parts = split(timePart, "-");
        if (parts.size() >= 3) {
            timePart = parts[0] + "-" + parts[1] + "-" + parts[2];
        } else {
            return 0;
        }
    }

    // Ghép sang định dạng parse: YYYY-MM-DD HH:MM:SS
    std::string full = datePart + " " + timePart;
    // Đổi dấu '-' trong phần giờ thành ':'
    std::replace(full.begin() + 11, full.end(), '-', ':');

    std::istringstream ss(full);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) return 0;

    time_t t = mktime(&tm);
    return t > 0 ? static_cast<uint64_t>(t) : 0;
}

static size_t removeExpiredSegment(const string &stream_path, uint64_t time_threshold) {
    size_t removed_bytes = 0;
    // find segment which create time is under time threshold
    unordered_map<string, size_t> remove_files;
    File::scanDir(stream_path, [&remove_files, time_threshold, stream_path](const string date_path, bool isDir) {
        if (isDir) {
            string date_string = findSubString(date_path.data() + stream_path.size(),"/", nullptr);
            auto date_time = findTimestampPath(date_string);
            if (time_threshold <= date_time) {
                return true;
            }
            File::scanDir(date_path, [&remove_files, time_threshold, stream_path](const string path, bool isDir) {
                if (!isDir && end_with(path, ".mp4")) {
                    string relative_path = findSubString(path.data() + stream_path.size(), "/", ".mp4");
                    auto start_time = findTimestampPath(relative_path);
                    if (time_threshold <= start_time) {
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
    GET_CONFIG(uint32_t, s_max_second, Protocol::kMP4MaxSecond);
    auto record_path = File::absolutePath(appName, mp4_save_path);
    unordered_map<string, uint64_t> path_threshold;
    File::scanDir(record_path, [&](const string path, bool isDir) {
        if (isDir) {
            auto sub_path = findSubString(path.data() + record_path.size(), "/", nullptr);
            auto tuples = split(sub_path, "/");
            if (tuples.size() == 2) {
                string camera_id = tuples[0];
                string stream_id = tuples[1];
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
        DebugL << "Remove expired file: " << it.first << ". Threshold: " << (it.second ? getTimeStr("%Y-%m-%d %H:%M:%S", it.second) : 0) << ". Removed bytes: " << format_bytes_human_readable(bytes);
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

void StorageManager::enforceStoragePolicy() {
    // estimate removed space need to reclaim
    size_t space_reclaim = estimateSpaceToReclaim();

    // rewrite time file with time rebuilder
    KeepTimeMap keep_time_map;
    size_t removed_bytes = recreateTimeFile(keep_time_map, space_reclaim);
    if (!removed_bytes) {
        return;
    }

    // asynchronous delete expired segment by scanning folder and removing file which start time over threshold
    weak_ptr<StorageManager> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self, keep_time_map]() {
        // Switch back to your own thread
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        // Reset a timer to track the operation time of a cycle
        strong_self->_ticker.resetTime();

        auto removed_volume_bytes = removeExpiredSegment(keep_time_map);
        InfoL << "Finished enforcing storage policy: " << format_bytes_human_readable(removed_volume_bytes) << ". Elapsed: " << formatDuration(strong_self->_ticker.elapsedTime());

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
        600.0f,
        [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            strong_self->enforceStoragePolicy();
            strong_self->removeExpiredUserSession();
            return true;
        },
        _poller);
}

string StorageManager::getMainStorageMountPoint() {
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    string record_path = File::absolutePath(app_name, mp4_save_path);
    return findMountPoint(record_path);
}

void StorageManager::getMainStorageUsage(size_t &used_bytes, size_t &total_bytes) {
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    string record_path = File::absolutePath(app_name, mp4_save_path);
    double usage_pct = 0.0;
    if (!findMountPoint(record_path, usage_pct, used_bytes, total_bytes)) {
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

} // namespace managerkit