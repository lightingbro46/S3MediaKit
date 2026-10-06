#include <ctime>
#include <cstring>
#include <cmath>
#include <deque>
#include <cctype>
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>

#include "Util/base64.h"
#include "Util/util.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Common/StrUtil.h"
#include "Common/BlockingExecutors.h"
#include "Server/GlobalMonitor.h"
#include "Local/StatisticRecorder.h"
#include "Local/TierEncryptionManager.h"
#include "Local/TierEncryptionUtils.h"
#include "Local/TimeDemuxer.h"
#include "Local/TimeQuery.h"
#include "Local/TimeRebuilder.h"
#include "Local/MotionBlockManager.h"
#include "Common/DeviceSource.h"
#include "Storage/Bookmark.h"
#include "Storage/TierBackfillTask.h"
#include "Extension/Resource.h"
#include "Storage/TierQualityJob.h"
#include "Local/TierMediaTransformer.h"

#include "TierStorageManager.h"
#include "TierStorageBase.h"
#include "StorageManager.h"

#ifdef ENABLE_TIER_STORAGE

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

namespace {

std::string normalizeStoragePath(const std::string &path) {
    return TierFileStorageBase::normalizeBasePath(path);
}

std::string appendStoragePath(const std::string &root, const std::string &child) {
    if (root.empty() || child.empty())
        return normalizeStoragePath(root);
    return File::absolutePath(child, root);
}

std::string configuredPoolDataRoot(const StoragePool &pool) {
    GET_CONFIG(std::string, app_name, Record::kAppName);
    return TierStorageManager::resolveFilePoolDataRoot(pool, app_name);
}

std::string tierJournalIdentity(const std::string &stream_id,
                                const std::string &segment_path) {
    return stream_id + "\n" + segment_path;
}

} // namespace

Json::Value StorageMountCandidate::toJson() const {
    Json::Value value;
    value["name"] = device;
    value["mount"] = mount_path;
    value["actual_mount"] = actual_mount_path;
    value["data_path"] = data_path;
    value["filesystem_type"] = filesystem_type;
    value["pool_type"] = pool_type;
    value["layout"] = layout;
    value["used"] = static_cast<Json::UInt64>(used_bytes);
    value["free"] = static_cast<Json::UInt64>(free_bytes);
    value["total"] = static_cast<Json::UInt64>(total_bytes);
    value["used_pct"] = sanitize_for_json(usage_pct);
    return value;
}

StorageMountCandidate StorageMountCandidate::fromDiskPartition(
        const DiskPartition &partition,
        const std::string &candidate_pool_type,
        const std::string &app_name) {
    StorageMountCandidate candidate;
    candidate.device = partition.device;
    candidate.actual_mount_path = normalizeStoragePath(partition.mount_point);
    candidate.mount_path = candidate.actual_mount_path;
    candidate.filesystem_type = partition.filesystem_type;
    candidate.pool_type = candidate_pool_type;
    candidate.layout = "DIRECT_ROOT";
    candidate.total_bytes = partition.total_bytes;
    candidate.free_bytes = partition.free_bytes;
    candidate.used_bytes = partition.used_bytes;
    candidate.usage_pct = partition.usage_pct;

    if (!app_name.empty()) {
        const std::string suffix = "/" + app_name;
        if (candidate.actual_mount_path == suffix) {
            candidate.mount_path = "/";
            candidate.layout = "APP_DIRECTORY_MOUNT";
        } else if (candidate.actual_mount_path.size() > suffix.size() &&
                   candidate.actual_mount_path.compare(
                       candidate.actual_mount_path.size() - suffix.size(),
                       suffix.size(), suffix) == 0) {
            candidate.mount_path.resize(candidate.mount_path.size() - suffix.size());
            candidate.layout = "APP_DIRECTORY_MOUNT";
        }
    }
    candidate.data_path = appendStoragePath(candidate.mount_path, app_name);
    return candidate;
}

std::string TierStorageManager::resolveFilePoolMountRoot(const StoragePool &pool) {
    return normalizeStoragePath(pool.mount_path.value_or(pool.network_path.value_or("")));
}

std::string TierStorageManager::resolveFilePoolDataRoot(const StoragePool &pool,
                                                        const std::string &app_name) {
    return appendStoragePath(resolveFilePoolMountRoot(pool), app_name);
}

INSTANCE_IMP(TierStorageManager)

namespace {

// Set while any pool has record encryption enabled. Keeps the ".enc" probing off the
// hot path for deployments that never turn encryption on.
static std::atomic<bool> s_encryption_active{false};

static bool regularFileExists(const std::string &p) {
    if (p.empty())
        return false;
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Returns whichever variant of `path` is really on disk: the encrypted one wins.
static std::string resolveLocalVariant(const std::string &path) {
    if (path.empty() || !s_encryption_active.load(std::memory_order_relaxed))
        return path;
    const std::string plain = RecordEncryptionUtils::stripEncSuffix(path);
    const std::string enc = plain + RecordEncryptionUtils::encryptedFileSuffix();
    if (regularFileExists(enc))
        return enc;
    if (regularFileExists(plain))
        return plain;
    return path;
}

// Makes a storage key describe the variant actually being written, so an encrypted
// file is never stored under the plaintext key (which would orphan the ".enc" copy).
static std::string keyForVariant(const std::string &key, const std::string &local_path) {
    if (!s_encryption_active.load(std::memory_order_relaxed))
        return key;
    if (!RecordEncryptionUtils::hasEncSuffix(local_path))
        return RecordEncryptionUtils::stripEncSuffix(key);
    return RecordEncryptionUtils::hasEncSuffix(key) ? key : key + RecordEncryptionUtils::encryptedFileSuffix();
}

// An object pool holds one of two spellings for the same segment: the plaintext key and,
// once record encryption is on, that key plus ".enc". A reader that rebuilds the key from the
// timefile only knows the plaintext spelling, so both must count as present; otherwise every
// encrypted segment looks lost. Returns the entry found, and its spelling in `out_key`.
static const TierObjectInfo *findListedObjectVariant(const std::map<std::string, TierObjectInfo> &listed,
                                                     const std::string &plain_key,
                                                     std::string &out_key) {
    auto it = listed.find(plain_key);
    if (it == listed.end())
        it = listed.find(plain_key + RecordEncryptionUtils::encryptedFileSuffix());
    if (it == listed.end()) {
        out_key.clear();
        return nullptr;
    }
    out_key = it->first;
    return &it->second;
}

// The encrypted variant carries a fixed-size trailer on top of the recorded payload, so its
// object is larger than the size the timefile stores for the segment.
static bool objectSizeMatchesSegment(int64_t object_size, int64_t segment_size, bool encrypted_variant) {
    if (segment_size <= 0 || object_size == segment_size)
        return true;
    return encrypted_variant &&
           object_size == segment_size + static_cast<int64_t>(EncryptedFileTrailer::kSize);
}

static const char kDefaultHotPoolId[] = "pool-default-hot";
static const char kDefaultHotPoolName[] = "Default HOT Tier Storage";
static const char kSystemDefaultPolicyId[] = "policy-system-default";
static const char kSystemDefaultPolicyName[] = "System Default Policy";
static const char kMissingSourceAndTargetError[] = "SOURCE_AND_TARGET_NOT_FOUND";

static int getMissingSegmentMaxAttempts() {
    GET_CONFIG(int, max_attempts, Storage::kTieringMissingSegmentMaxAttempts);
    return max_attempts > 0 ? max_attempts : 3;
}

static int countMissingSegmentFailures(const std::string &range_id) {
    return static_cast<int>(TieringJobImp().findRecentFailedByRange(
        range_id, kMissingSourceAndTargetError, getMissingSegmentMaxAttempts()).size());
}

// Earliest time a range whose previous jobs hit missing segments may be
// queued again. Returns false when the range has no such failures.
static bool missingSegmentRetryAt(const std::string &range_id, int64_t &retry_at) {
    retry_at = 0;
    auto failed = TieringJobImp().findRecentFailedByRange(
        range_id, kMissingSourceAndTargetError, getMissingSegmentMaxAttempts());
    if (failed.empty())
        return false;
    GET_CONFIG(int64_t, base_seconds, Storage::kTieringMissingSegmentRetryBaseSeconds);
    const int64_t max_delay = 86400;
    if (base_seconds <= 0)
        base_seconds = 3600;
    base_seconds = std::min(base_seconds, max_delay);
    const int shift = std::min<int>(static_cast<int>(failed.size()) - 1, 16);
    const int64_t delay = std::min(base_seconds << shift, max_delay);
    retry_at = failed.front().updated_at + delay;
    return true;
}

static uint64_t nextLocalDayStart(uint64_t day_start) {
    return StampUtils::getStartOfDay(day_start + 36 * 3600);
}

static std::vector<uint64_t> localDaysForWindow(int64_t start_time, int64_t end_time) {
    std::vector<uint64_t> days;
    if (start_time <= 0 || end_time < start_time)
        return days;
    const uint64_t exclusive_end = static_cast<uint64_t>(
        end_time == start_time ? end_time + 1 : end_time);
    uint64_t day = StampUtils::getStartOfDay(static_cast<uint64_t>(start_time));
    while (day < exclusive_end) {
        days.push_back(day);
        auto next = nextLocalDayStart(day);
        if (next <= day)
            break;
        day = next;
    }
    return days;
}

struct DiskSegmentInfo {
    std::string camera_id;
    std::string stream_id;
    std::string segment_path;
    std::string full_path;
    int64_t start_time = 0;
    int64_t end_time = 0;
    int64_t file_size = 0;
};

static std::vector<DeviceTuple> collectGenericRtspCameraSources() {
    std::vector<DeviceTuple> tuples;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &device) {
        if (!device)
            return;
        const auto &tuple = device->getDeviceTuple();
        if (!tuple.device_id.empty())
            tuples.emplace_back(tuple);
    }, GENERIC_RTSP_CAMERA_SCHEMA);

    return tuples;
}

static std::vector<DiskSegmentInfo> collectDiskSegmentsFromRoot(const std::string &record_root,
                                                                const std::string &camera_id,
                                                                int64_t start_time,
                                                                int64_t end_time) {
    std::vector<DiskSegmentInfo> ret;
    std::string camera_root = record_root + "/" + camera_id;
    if (!File::is_dir(camera_root)) {
        return ret;
    }

    File::scanDir(camera_root, [&](const std::string &path, bool isDir) {
        // Matching only ".mp4" made every encrypted recording invisible to the
        // restore-cache cleanup, so those files were never reclaimed.
        if (isDir || !end_with(RecordEncryptionUtils::stripEncSuffix(path), ".mp4"))
            return true;
        std::string rel = path.substr(camera_root.size());
        if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
        auto slash = rel.find('/');
        if (slash == std::string::npos)
            return true;

        std::string stream_id = rel.substr(0, slash);
        std::string segment_path = rel.substr(slash + 1);
        if (segment_path.size() > 4)
            segment_path.resize(segment_path.size() - 4);

        int64_t ts = static_cast<int64_t>(StrTimeUtils::getTsFromDateTimeStr(segment_path));
        if (ts <= 0)
            ts = static_cast<int64_t>(StrTimeUtils::getTsFromDateTimeStr2(segment_path));
        if (ts <= 0)
            return true;

        int64_t seg_end = ts + 60;
        if (end_time > 0 && ts > end_time)
            return true;
        if (start_time > 0 && seg_end < start_time)
            return true;

        DiskSegmentInfo item;
        item.camera_id = camera_id;
        item.stream_id = stream_id;
        item.segment_path = segment_path;
        item.full_path = path;
        item.start_time = ts;
        item.end_time = seg_end;
        item.file_size = static_cast<int64_t>(File::fileSize(path));
        ret.push_back(std::move(item));
        return true;
    }, false, true);

    std::sort(ret.begin(), ret.end(), [](const DiskSegmentInfo &a, const DiskSegmentInfo &b) {
        return a.start_time < b.start_time;
    });
    return ret;
}

static bool pathStartsWithRoot(const std::string &path, const std::string &root) {
    if (path.empty() || root.empty())
        return false;
    if (!start_with(path, root))
        return false;
    return path.size() == root.size() || path[root.size()] == '/';
}

static std::string parentPath(const std::string &path) {
    auto pos = path.rfind('/');
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

static bool isDateDirectoryName(const std::string &name) {
    if (name.size() != 10 || name[4] != '-' || name[7] != '-')
        return false;
    for (size_t i = 0; i < name.size(); ++i) {
        if (i == 4 || i == 7)
            continue;
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
            return false;
    }
    return true;
}

static void pruneEmptySegmentDateDirectories(
        const std::string &storage_root,
        const std::vector<TierRangeSegmentFile> &segments) {
    const std::string root = normalizeStoragePath(storage_root);
    if (root.empty())
        return;

    std::unordered_set<std::string> candidates;
    for (const auto &segment : segments) {
        const std::string date_dir = parentPath(segment.full_path);
        const std::string stream_root = root + "/" + segment.camera_id + "/" + segment.stream_id;
        if (!pathStartsWithRoot(date_dir, stream_root) || date_dir.size() <= stream_root.size() + 1)
            continue;
        const std::string relative = date_dir.substr(stream_root.size() + 1);
        if (relative.find('/') == std::string::npos && isDateDirectoryName(relative))
            candidates.insert(date_dir);
    }
    for (const auto &date_dir : candidates)
        File::deleteEmptyDir(date_dir, false);
}

static bool deriveSegmentPathFromFullPath(const std::string &full_path,
                                          const std::string &camera_id,
                                          const std::string &stream_id,
                                          std::string &segment_path) {
    if (full_path.empty() || camera_id.empty() || stream_id.empty())
        return false;
    std::string marker = "/" + camera_id + "/" + stream_id + "/";
    auto pos = full_path.rfind(marker);
    if (pos == std::string::npos)
        return false;
    segment_path = full_path.substr(pos + marker.size());
    // An encrypted segment keeps the same storage key as its plaintext form.
    segment_path = RecordEncryptionUtils::stripEncSuffix(segment_path);
    if (!end_with(segment_path, ".mp4"))
        return false;
    segment_path.resize(segment_path.size() - 4);
    return !segment_path.empty();
}

// Floors an age threshold to the split granularity so tier moves and
// retention cut ranges once per interval rather than on every cycle.
static int64_t alignTierThreshold(int64_t threshold) {
    GET_CONFIG(int64_t, granularity, Storage::kTieringSplitGranularitySeconds);
    if (granularity <= 1 || threshold <= 0)
        return threshold;
    return threshold - threshold % granularity;
}

static std::string cameraTimelineDir(const std::string &camera_id) {
    GET_CONFIG(std::string, record_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    return File::absolutePath(app_name, record_path) + "/" + camera_id;
}

// Daily timefiles hold blocks by the local date of their start. Returns the
// ';'-joined files that can hold blocks starting in [start_time, end_time],
// plus any file whose name is not a date. Opening only these keeps a lookup
// from touching every day of a camera's retention window.
static std::string timelineFilesForWindow(const std::string &timeline_dir,
                                          int64_t start_time,
                                          int64_t end_time) {
    const uint64_t first_day = StampUtils::getStartOfDay(static_cast<uint64_t>(std::max<int64_t>(0, start_time)));
    const uint64_t last_day = StampUtils::getStartOfDay(static_cast<uint64_t>(std::max<int64_t>(0, end_time)));
    std::vector<std::string> files;
    File::scanDir(timeline_dir, [&](const std::string &path, bool is_dir) {
        if (is_dir || !end_with(path, ".s3db"))
            return true;
        auto slash = path.rfind('/');
        std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        name.resize(name.size() - 5);
        const uint64_t day = StrTimeUtils::getTsFromDateStr(name);
        if (day == 0 || (day >= first_day && day <= last_day))
            files.push_back(path);
        return true;
    });
    std::sort(files.begin(), files.end());
    std::string joined;
    for (const auto &file : files) {
        if (!joined.empty())
            joined += ";";
        joined += file;
    }
    return joined;
}

static std::string getRestoreRootPath() {
    GET_CONFIG(std::string, restore_save_path, Storage::kRestoreSavePath);
    return File::absolutePath("", restore_save_path);
}

static bool isSafeRestoreJobId(const std::string &job_id) {
    if (job_id.empty() || job_id == "." || job_id == "..")
        return false;
    for (const auto ch : job_id) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_')
            return false;
    }
    return true;
}

static std::string buildRestoreJobRootPath(const std::string &job_id) {
    if (!isSafeRestoreJobId(job_id))
        return "";
    auto root = getRestoreRootPath();
    if (!root.empty() && root.back() == '/')
        root.pop_back();
    return root + "/" + job_id;
}

static std::string buildLegacyRestoreSegmentPath(const std::string &camera_id,
                                           const std::string &stream_id,
                                           const std::string &segment_path) {
    auto root = getRestoreRootPath();
    if (!root.empty() && root.back() == '/')
        root.pop_back();
    return root + "/" + camera_id + "/" + stream_id + "/" + segment_path + ".mp4";
}

static std::string buildRestoreSegmentPath(const std::string &job_id,
                                           const std::string &camera_id,
                                           const std::string &stream_id,
                                           const std::string &segment_path) {
    auto job_root = buildRestoreJobRootPath(job_id);
    if (job_root.empty())
        return "";
    return job_root + "/" + camera_id + "/" + stream_id + "/" + segment_path + ".mp4";
}

static bool isJobScopedRestoreLayout(const std::string &job_id) {
    auto job_root = buildRestoreJobRootPath(job_id);
    return !job_root.empty() && File::is_dir(job_root);
}

static std::string resolveRestoreSegmentPath(const RestoreJob &job,
                                             const RestoreJobSegment &segment) {
    if (isJobScopedRestoreLayout(job.job_id))
        return resolveLocalVariant(buildRestoreSegmentPath(job.job_id, segment.camera_id,
                                                           segment.stream_id, segment.segment_path));
    return resolveLocalVariant(buildLegacyRestoreSegmentPath(segment.camera_id, segment.stream_id,
                                                             segment.segment_path));
}

// Restore children run concurrently and commonly create the same camera/stream
// directory. File::create_path() performs access()+mkdir(), so without this
// narrow lock a losing thread treats the expected EEXIST race as a failure.
static std::mutex s_restore_directory_mtx;

static bool ensureDirectoryPath(const std::string &dir, unsigned int mode, std::string &err) {
    if (dir.empty()) {
        err = "Directory path is empty";
        return false;
    }
    std::lock_guard<std::mutex> directory_lk(s_restore_directory_mtx);
    std::string mkdir_path = dir;
    if (mkdir_path.back() != '/')
        mkdir_path += '/';
    if (!File::create_path(mkdir_path, mode)) {
        err = "Cannot create directory: " + dir;
        return false;
    }
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
        err = std::string("Cannot stat directory: ") + dir + ": " + strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        err = "Path is not a directory: " + dir;
        return false;
    }
    return true;
}

static bool prepareRestoreJobDirectory(const std::string &job_id, std::string &err) {
    const auto job_root = buildRestoreJobRootPath(job_id);
    if (job_root.empty()) {
        err = "Invalid restore job id";
        return false;
    }
    if (!ensureDirectoryPath(job_root, S_IRWXO | S_IRWXG | S_IRWXU, err))
        return false;
    const std::string marker_path = job_root + "/.restore_job";
    if (!File::saveFile(job_id + "\n", marker_path)) {
        err = "Cannot create restore job marker: " + marker_path;
        File::delete_file(job_root, false, false);
        return false;
    }
    return true;
}

static std::set<std::string> restoreSegmentIdentities(
        const std::vector<RestoreJobSegment> &segments) {
    std::set<std::string> identities;
    for (const auto &segment : segments) {
        identities.insert(segment.camera_id + "\n" + segment.stream_id + "\n" +
                          segment.range_id + "\n" + segment.segment_path + "\n" +
                          segment.source_pool_id + "\n" +
                          std::to_string(segment.range_start) + "\n" +
                          std::to_string(segment.range_end));
    }
    return identities;
}

static bool sameRangeSnapshot(const SegmentTierRange &a, const SegmentTierRange &b) {
    return a.range_id == b.range_id && a.camera_id == b.camera_id &&
           a.stream_id == b.stream_id && a.tier == b.tier &&
           a.pool_id == b.pool_id && a.status == b.status &&
           a.start_time == b.start_time && a.end_time == b.end_time &&
           a.segment_count == b.segment_count && a.size_bytes == b.size_bytes;
}

struct ShapedPiece {
    SegmentTierRange range;
    int kind = 0;
};

// Rebuilds a range from its segments (sorted by start). Each segment has a
// kind; kind 0 drops it. Segments sharing a start cannot be separated by
// ownership, so a group takes the largest kind of its members, while stats
// count only members with a non-zero kind. Consecutive groups of the same kind
// form one piece unless `break_gap(previous_end, next_start)` says otherwise.
// Piece windows are tightened to their own segments and never reach the next
// group's start, so a piece never owns a segment of another piece.
static bool shapeRangePieces(const SegmentTierRange &range,
                             const std::vector<TierRangeSegmentFile> &segments,
                             const std::vector<int> &kinds,
                             const std::function<bool(int64_t, int64_t)> &break_gap,
                             int64_t now,
                             std::vector<ShapedPiece> &out) {
    out.clear();
    struct Group {
        size_t first = 0;
        size_t last = 0;
        int kind = 0;
    };
    std::vector<Group> groups;
    for (size_t index = 0; index < segments.size();) {
        Group group;
        group.first = index;
        while (index < segments.size() &&
               segments[index].start_time == segments[group.first].start_time) {
            group.kind = std::max(group.kind, kinds[index]);
            ++index;
        }
        group.last = index;
        groups.push_back(group);
    }

    bool open = false;
    int64_t open_end = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        const auto &group = groups[g];
        const int64_t group_start = segments[group.first].start_time;
        if (open) {
            // Never let a piece reach the start of the following group.
            out.back().range.end_time = std::min(out.back().range.end_time, group_start);
        }
        const bool extend = open && group.kind != 0 && out.back().kind == group.kind &&
                            !(break_gap && break_gap(open_end, group_start));
        if (group.kind == 0) {
            open = false;
            continue;
        }
        if (!extend) {
            ShapedPiece piece;
            piece.kind = group.kind;
            piece.range = range;
            piece.range.range_id = out.empty() ? range.range_id : StrUUID::make_guid(8, "rng");
            piece.range.start_time = group_start;
            piece.range.end_time = group_start;
            piece.range.segment_count = 0;
            piece.range.size_bytes = 0;
            piece.range.created_at = out.empty() ? range.created_at : now;
            piece.range.updated_at = now;
            out.push_back(piece);
            open_end = group_start;
        }
        for (size_t i = group.first; i < group.last; ++i) {
            if (kinds[i] == 0)
                continue;
            auto &piece = out.back().range;
            piece.end_time = std::max(piece.end_time, segments[i].end_time);
            open_end = std::max(open_end, segments[i].end_time);
            ++piece.segment_count;
            piece.size_bytes += std::max<int64_t>(0, segments[i].file_size);
        }
        open = true;
    }
    if (!out.empty())
        out.back().range.end_time = std::min(out.back().range.end_time, range.end_time);
    for (const auto &piece : out) {
        if (piece.range.end_time <= piece.range.start_time)
            return false;
    }
    return true;
}

static bool isSameFileOnDisk(const std::string &a, const std::string &b) {
    if (a.empty() || b.empty())
        return false;
    if (a == b)
        return true;
    struct stat sa{};
    struct stat sb{};
    if (::stat(a.c_str(), &sa) != 0 || ::stat(b.c_str(), &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

} // namespace

// ===================================================================
// Lifecycle
// ===================================================================
TierStorageManager::TierStorageManager(const EventPoller::Ptr &poller) {
    _poller = poller ? poller : EventPollerPool::Instance().getPoller();
}

TierStorageManager::~TierStorageManager() {
    stop();
}

// ===================================================================
// Object-storage pool helpers
// ===================================================================

// Thin "decryption" shim — extend here if you encrypt secrets at rest.
std::string TierStorageManager::decryptPoolSecret(const std::string &encrypted) {
    return encrypted; // currently stored as plaintext
}

bool TierStorageManager::registerPoolObjStorage(const StoragePool &pool) {
    if (!poolTypeIsObjectStorage(pool.type)) return false;

    if (!pool.endpoint.has_value() || !pool.bucket.has_value() ||
        !pool.access_key.has_value() || !pool.secret_key_enc.has_value()) {
        WarnL << "Failed to register pool object storage: missing credentials for pool " << pool.id;
        return false;
    }

    std::string secret = decryptPoolSecret(pool.secret_key_enc.value());
    std::string base   = pool.base_path.has_value() ? pool.base_path.value() : "";

    const bool registered = _obj_storage.registerPool(
        pool.id, pool.type,
        pool.endpoint.value(),
        pool.bucket.value(),
        base,
        pool.access_key.value(),
        secret);
    if (registered)
        refreshPoolVersioning(pool, true);
    return registered;
}

int64_t TierStorageManager::quotaUsedBytes(const std::string &pool_id) {
    GET_CONFIG(int64_t, cache_seconds, Storage::kTieringQuotaUsageCacheSeconds);
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (cache_seconds > 0) {
        std::lock_guard<std::mutex> lk(_quota_usage_mtx);
        auto it = _quota_usage.find(pool_id);
        if (it != _quota_usage.end() && now - it->second.first < cache_seconds)
            return it->second.second;
    }
    const int64_t used = SegmentTierRangeImp().sumStoredBytesByPool(pool_id);
    std::lock_guard<std::mutex> lk(_quota_usage_mtx);
    _quota_usage[pool_id] = std::make_pair(now, used);
    return used;
}

std::string TierStorageManager::refreshPoolVersioning(const StoragePool &pool, bool force) {
    static const int64_t kCheckIntervalSeconds = 3600;
    static const int64_t kAlertIntervalSeconds = 86400;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    {
        std::lock_guard<std::mutex> lk(_pool_versioning_mtx);
        auto &state = _pool_versioning[pool.id];
        if (!force && now - state.checked_at < kCheckIntervalSeconds)
            return state.status;
        // Claim the check so concurrent callers do not repeat the request.
        state.checked_at = now;
    }

    std::string status;
    if (!_obj_storage.bucketVersioningStatus(pool.id, status)) {
        std::lock_guard<std::mutex> lk(_pool_versioning_mtx);
        return _pool_versioning[pool.id].status;
    }

    bool alert = false;
    {
        std::lock_guard<std::mutex> lk(_pool_versioning_mtx);
        auto &state = _pool_versioning[pool.id];
        state.status = status;
        if (status == "Enabled" && now - state.alerted_at >= kAlertIntervalSeconds) {
            state.alerted_at = now;
            alert = true;
        }
    }
    if (alert) {
        WarnL << "Bucket versioning is enabled on object pool " << pool.id
              << "; deleted recordings stay as noncurrent versions and do not free space";
        StorageAlert item;
        item.id = StrUUID::make_guid(8, "alert");
        item.level = "WARNING";
        item.type = "POOL_VERSIONING_BLOCKS_RECLAIM";
        item.pool_id = pool.id;
        item.message = "Bucket versioning is Enabled on bucket " + pool.bucket.value_or("") +
                       ": deleted recordings remain as noncurrent versions and still use "
                       "capacity. Add a lifecycle rule with NoncurrentVersionExpiration "
                       "(and expired delete-marker cleanup) or disable versioning.";
        item.created_at = now;
        StorageAlertImp().add(item);
    }
    return status;
}

void TierStorageManager::unregisterPoolObjStorage(const std::string &pool_id) {
    _obj_storage.unregisterPool(pool_id);
    std::lock_guard<std::mutex> lk(_pool_versioning_mtx);
    _pool_versioning.erase(pool_id);
    std::lock_guard<std::mutex> quota_lk(_quota_usage_mtx);
    _quota_usage.erase(pool_id);
}

TierFileStorageBase *TierStorageManager::fileStorageForPoolType(const std::string &type) {
    if (type == poolTypeToString(StoragePoolType::LOCAL_DISK))
        return &_local_storage;
    if (type == poolTypeToString(StoragePoolType::NAS))
        return &_nas_storage;
    return nullptr;
}

const TierFileStorageBase *TierStorageManager::fileStorageForPoolType(const std::string &type) const {
    if (type == poolTypeToString(StoragePoolType::LOCAL_DISK))
        return &_local_storage;
    if (type == poolTypeToString(StoragePoolType::NAS))
        return &_nas_storage;
    return nullptr;
}

bool TierStorageManager::registerPoolFileStorage(const StoragePool &pool) {
    if (poolTypeIsObjectStorage(pool.type))
        return false;

    auto storage = fileStorageForPoolType(pool.type);
    if (!storage) {
        WarnL << "registerPoolFileStorage: unsupported file pool type " << pool.type << " pool=" << pool.id;
        return false;
    }

    std::string path = configuredPoolDataRoot(pool);
    if (path.empty()) {
        WarnL << "registerPoolFileStorage: missing mount_path/network_path for pool " << pool.id;
        return false;
    }

    const std::string expected_mount =
        pool.type == poolTypeToString(StoragePoolType::NAS) && pool.health_check_enabled
            ? resolveFilePoolMountRoot(pool)
            : "";
    return storage->registerPool(pool.id, pool.type, path, expected_mount);
}

void TierStorageManager::unregisterPoolFileStorage(const std::string &pool_id) {
    _local_storage.unregisterPool(pool_id);
    _nas_storage.unregisterPool(pool_id);
}

std::string TierStorageManager::getSystemDefaultHotPoolId() const {
    StoragePoolImp imp;
    auto fixed = imp.findByPoolId(kDefaultHotPoolId);
    if (!fixed.empty() && fixed.front().enabled &&
        fixed.front().tier == tierTypeToString(HotTier) &&
        !poolTypeIsObjectStorage(fixed.front().type))
        return fixed.front().id;
    auto hot_pools = imp.queryAll(tierTypeToString(HotTier));
    std::sort(hot_pools.begin(), hot_pools.end(), [](const StoragePool &a, const StoragePool &b) {
        return a.id < b.id;
    });
    for (const auto &pool : hot_pools) {
        if (pool.enabled && !poolTypeIsObjectStorage(pool.type))
            return pool.id;
    }
    return "";
}

void TierStorageManager::ensureDefaultHotPool() {
    GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    if (mp4_save_path.empty()) {
        WarnL << "Cannot create default HOT pool, kMP4SavePath is empty";
        return;
    }
    std::string mount_root = File::absolutePath("", mp4_save_path);
    std::string record_path = File::absolutePath(app_name, mount_root);

    string message;
    int latency_ms = 0;
    auto storage = fileStorageForPoolType(poolTypeToString(StoragePoolType::LOCAL_DISK));
    if (!storage->testConnectionParams(record_path, message, latency_ms)) {
        WarnL << "Mount point " << record_path << " is not writable, error: " << message;
        return;
    }
    DebugL << "Mount point " << record_path << " is writable, latency: " << latency_ms << " ms";

    GET_CONFIG_FUNC(int, auto_hot_high_watermark, Storage::kDefaultHotHighWatermarkPercent, [](const std::string &str) {
        return str.empty() ? 80 : atoi(str.data());
    });
    GET_CONFIG_FUNC(int, auto_hot_critical_watermark, Storage::kDefaultHotCriticalWatermarkPercent, [](const std::string &str) {
        return str.empty() ? 90 : atoi(str.data());
    });
    if (auto_hot_high_watermark <= 0 || auto_hot_high_watermark >= 100)
        auto_hot_high_watermark = 85;
    if (auto_hot_critical_watermark <= auto_hot_high_watermark || auto_hot_critical_watermark > 95)
        auto_hot_critical_watermark = std::min(95, std::max(auto_hot_high_watermark + 1, 90));

    StoragePoolImp imp;
    const std::string selected_hot_pool = getSystemDefaultHotPoolId();
    if (!selected_hot_pool.empty()) {
        InfoL << "Use existing HOT pool as system default: " << selected_hot_pool;
        return;
    }

    {
        StoragePool pool;
        pool.id   = kDefaultHotPoolId;
        pool.name = kDefaultHotPoolName;
        pool.type = poolTypeToString(StoragePoolType::LOCAL_DISK);
        pool.tier = tierTypeToString(HotTier);
        pool.mount_path = Optional<std::string>(mount_root);
        pool.enabled = 1;
        pool.health_check_enabled = 1;
        pool.high_watermark_percent = auto_hot_high_watermark;
        pool.critical_watermark_percent = auto_hot_critical_watermark;
        if (createPool(pool).empty()) {
            WarnL << "Failed to create default HOT pool at " << mount_root;
            return;
        }
        InfoL << "Created default HOT pool " << pool.id << " path=" << mount_root;
    }
}

std::string TierStorageManager::getSystemDefaultPolicyId() const {
    return kSystemDefaultPolicyId;
}

void TierStorageManager::ensureSystemDefaultPolicy() {
    StoragePolicyImp policy_imp;
    auto existing = policy_imp.findByPolicyId(kSystemDefaultPolicyId);
    if (!existing.empty()) {
        auto policy = existing.front();
        bool changed = false;
        if (policy.name.empty()) {
            policy.name = kSystemDefaultPolicyName;
            changed = true;
        }
        if (policy.enabled == 0) {
            policy.enabled = 1;
            changed = true;
        }
        const std::string hot_pool_id = getSystemDefaultHotPoolId();
        auto tiers = policy.parsedTiers();
        for (auto &tier : tiers) {
            if (tier.tier == tierTypeToString(HotTier) && !hot_pool_id.empty() &&
                tier.pool_id != hot_pool_id) {
                tier.pool_id = hot_pool_id;
                changed = true;
            }
        }
        if (changed)
            policy.tiers_json = StoragePolicy::tiersToJson(tiers);
        if (changed) {
            policy.updated_at = static_cast<int64_t>(time(nullptr));
            if (!policy_imp.update(policy))
                WarnL << "Failed to update system default policy";
        }
        {
            std::lock_guard<std::mutex> lk(_default_policy_mtx);
            _default_policy_id = kSystemDefaultPolicyId;
        }
        return;
    }

    StoragePool default_hot_pool;
    {
        StoragePoolImp pool_imp;
        auto hot_pools = pool_imp.findByPoolId(getSystemDefaultHotPoolId());
        if (hot_pools.empty()) {
            WarnL << "Default HOT pool not found, cannot create system default policy";
            return;
        }
        default_hot_pool = hot_pools.front();
    }

    PolicyTierConfig hot_tier = getSystemDefaultPolicyTierConfig();
    hot_tier.pool_id = default_hot_pool.id;
    hot_tier.high_watermark_percent = default_hot_pool.high_watermark_percent;
    hot_tier.critical_watermark_percent = default_hot_pool.critical_watermark_percent;

    StoragePolicy policy;
    policy.id = kSystemDefaultPolicyId;
    policy.name = kSystemDefaultPolicyName;
    policy.description = Optional<std::string>("Built-in system default storage policy");
    policy.enabled = 1;
    policy.total_retention_days = hot_tier.retain_until_days;
    policy.allow_camera_override = 1;
    policy.protect_event_video = 0;
    policy.tiers_json = StoragePolicy::tiersToJson({hot_tier});
    policy.delete_policy_json = getSystemDefaultPolicyDeleteConfig().toJson().toStyledString();
    policy.advanced_rules_json = getSystemDefaultPolicyAdvancedRules().toJson().toStyledString();
    policy.created_at = static_cast<int64_t>(time(nullptr));
    policy.updated_at = policy.created_at;

    if (!policy_imp.add(policy)) {
        WarnL << "Failed to create system default policy";
        return;
    }
    {
        std::lock_guard<std::mutex> lk(_default_policy_mtx);
        _default_policy_id = kSystemDefaultPolicyId;
    }
    InfoL << "Created system default policy " << kSystemDefaultPolicyId << " with HOT pool " << default_hot_pool.id;
}

// ===================================================================
// Lifecycle
// ===================================================================
void TierStorageManager::start() {

    if (_timer) {
        WarnL << "TierStorageManager already running. Ignore";
        return;
    }

    _stopping = false;

    if (mINI::Instance()[Storage::kTierTranscodeEnabled].as<bool>()) {
        std::shared_ptr<TransferDispatchState> state = _transfer_dispatch_state;
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            ++state->callbacks_in_flight;
        }
        BlockingExecutors::tierTransfer()->async([state]() {
            try {
                TierEncoderCapabilityRegistry::Instance().probe();
            } catch (const std::exception &ex) {
                WarnL << "Tier encoder capability probe failed: " << ex.what();
            } catch (...) {
                WarnL << "Tier encoder capability probe failed";
            }
            std::lock_guard<std::mutex> lk(state->mutex);
            --state->callbacks_in_flight;
            state->completed.notify_all();
        }, false);
    }

    // Jobs left RUNNING by a crash are safe to claim again from durable state.
    TieringJobImp().recoverRunning();
    TieringJobSegmentImp().recoverTransforming();
    RestoreJobSegmentImp().recoverRunning();
    TierBackfillTaskImp().recoverRunning();

    // Refresh pool cache immediately on start
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        StoragePoolImp imp;
        for (const auto &p : imp.queryAll()) {
            _pool_cache[p.id] = p;
            if (p.enabled) {
                if (poolTypeIsObjectStorage(p.type))
                    registerPoolObjStorage(p);
                else
                    registerPoolFileStorage(p);
            }
        }
    }

    ensureDefaultHotPool();
    ensureSystemDefaultPolicy();

    {
        StoragePoolImp imp;
        std::lock_guard<std::mutex> lk(_pool_health_mtx);
        _pool_health_cache.clear();
        for (const auto &p : imp.queryAll()) {
            if (!p.enabled || !p.health_check_enabled)
                continue;
            auto &state = _pool_health_cache[p.id];
            state.info.pool_id = p.id;
            state.info.health_status = healthToString(StorageHealth::UNKNOWN);
            state.info.stale = true;
        }
    }

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    // Run every 5 minutes (300 s)
    _timer = std::make_shared<Timer>(
        300.0f,
        [weak_self]() -> bool {
            auto self = weak_self.lock();
            if (!self) return false;
            self->runTieringCycle();
            return true;
        },
        _poller);

    GET_CONFIG(float, backfill_dispatch_interval,
               Storage::kTierRangeBackfillDispatchIntervalSeconds);
    if (backfill_dispatch_interval <= 0)
        backfill_dispatch_interval = 1.0f;
    _backfill_tasks_initialized = false;
    _backfill_sync_ticker.resetTime();
    _backfill_timer = std::make_shared<Timer>(
        backfill_dispatch_interval,
        [weak_self]() -> bool {
            auto self = weak_self.lock();
            if (!self)
                return false;
            self->runBackfillCycle();
            return true;
        },
        _poller);

    GET_CONFIG(float, health_interval, Storage::kPoolHealthCheckIntervalSeconds);
    if (health_interval <= 0)
        health_interval = 30.0f;
    _health_timer = std::make_shared<Timer>(
        health_interval,
        [weak_self]() -> bool {
            auto self = weak_self.lock();
            if (!self)
                return false;
            self->runPoolHealthCycle();
            return true;
        },
        _poller);
    runPoolHealthCycle();
    dispatchPendingTieringJobs();

    InfoL << "TierStorageManager started";
}

void TierStorageManager::stop() {
    _stopping = true;
    _timer.reset();
    _backfill_timer.reset();
    _health_timer.reset();
    {
        std::lock_guard<std::mutex> lk(_tiering_cancellation_mtx);
        for (const auto &entry : _tiering_cancellations)
            entry.second->cancel();
    }
    // Object storage calls in flight (reconcile, upload, download) must finish before shutdown()
    // tears down the AWS SDK.
    waitForBackgroundWork();
    _backfill_tasks_initialized = false;
    {
        std::lock_guard<std::mutex> lk(_temporary_mp4_observations_mtx);
        _temporary_mp4_observations.clear();
        _temporary_mp4_next_pool_index = 0;
    }
    InfoL << "TierStorageManager stopped";
}

void TierStorageManager::waitForBackgroundWork() {
    // A cycle can queue transfers and restores, and a finished transfer queues the next one, so
    // repeat until one pass finds every group idle without having to wait.
    bool waited = true;
    while (waited) {
        waited = false;
        auto wait_tasks = [&waited](const std::shared_ptr<InFlightTasks> &tasks) {
            std::unique_lock<std::mutex> lk(tasks->mutex);
            while (tasks->tasks_in_flight > 0) {
                waited = true;
                tasks->completed.wait(lk);
            }
        };
        wait_tasks(_cycle_tasks);
        {
            std::unique_lock<std::mutex> lk(_health_cycle_state->mutex);
            while (_health_cycle_state->running) {
                waited = true;
                _health_cycle_state->completed.wait(lk);
            }
        }
        {
            std::unique_lock<std::mutex> lk(_transfer_dispatch_state->mutex);
            while (_transfer_dispatch_state->callbacks_in_flight > 0) {
                waited = true;
                _transfer_dispatch_state->completed.wait(lk);
            }
        }
        wait_tasks(_restore_dispatch_state);
    }
}

void TierStorageManager::shutdown() {
    stop();
    _obj_storage.shutdownSdk();
}

bool TierStorageManager::resolveHotPoolForPath(const std::string &file_path, StoragePool &out_pool, std::string &out_pool_root) const {
    std::string normalized_file = TierFileStorageBase::normalizeBasePath(file_path);
    size_t best_len = 0;
    bool found = false;

    std::lock_guard<std::mutex> lk(_pool_cache_mtx);
    for (const auto &kv : _pool_cache) {
        const auto &pool = kv.second;
        if (!pool.enabled)
            continue;
        if (pool.tier != tierTypeToString(HotTier))
            continue;
        if (poolTypeIsObjectStorage(pool.type))
            continue;

        std::vector<std::string> roots;
        const std::string data_root = configuredPoolDataRoot(pool);
        if (!data_root.empty())
            roots.push_back(data_root);

        for (const auto &raw_root : roots) {
            std::string root = TierFileStorageBase::normalizeBasePath(raw_root);
            if (!pathStartsWithRoot(normalized_file, root))
                continue;
            if (root.size() > best_len) {
                best_len = root.size();
                out_pool = pool;
                out_pool_root = root;
                found = true;
            }
        }
    }
    return found;
}

bool TierStorageManager::registerHotSegmentRange(const std::string &camera_id,
                                                 const std::string &stream_id,
                                                 const std::string &file_path,
                                                 int64_t start_time,
                                                 int64_t end_time,
                                                 int64_t file_size) {

    if (camera_id.empty() || stream_id.empty() || file_path.empty() || start_time <= 0) {
        WarnL << "Invalid hot segment parameters, camera=" << camera_id
              << " stream=" << stream_id
              << " file=" << file_path
              << " start_time=" << start_time
              << " end_time=" << end_time
              << " file_size=" << file_size;
        return false;
    }
    if (end_time <= start_time) {
        GET_CONFIG(int, max_second, Protocol::kMP4MaxSecond)
        end_time = start_time + max_second;
    }

    StoragePool hot_pool;
    std::string hot_root;
    if (!resolveHotPoolForPath(file_path, hot_pool, hot_root)) {
        WarnL << "No HOT pool matched file " << file_path << ". Ignore hot segment";
        return false;
    }

    if (file_size <= 0) {
        file_size = static_cast<int64_t>(File::fileSize(file_path));
    }
    if (file_size > 0)
        _throughput.addIngest(camera_id, hot_pool.id, static_cast<uint64_t>(file_size), steadyNowMs());

    SegmentTierRangeImp range_imp;
    std::string hot_tier = tierTypeToString(HotTier);
    std::string available = segmentStatusToString(SegmentStatus::AVAILABLE);
    std::lock_guard<std::mutex> registration_lk(_range_registration_mtx);

    int64_t now = static_cast<int64_t>(time(nullptr));
    SegmentTierRange range;
    range.range_id = StrUUID::make_guid(8, "rng");
    range.camera_id = camera_id;
    range.stream_id = stream_id;
    range.tier = hot_tier;
    range.pool_id = hot_pool.id;
    range.status = available;
    range.start_time = start_time;
    range.end_time = end_time;
    range.segment_count = 1;
    range.size_bytes = std::max<int64_t>(0, file_size);
    range.created_at = now;
    range.updated_at = now;

    bool ok = range_imp.registerIfUncovered(range);
    if (ok) {
        DebugL << "Registered HOT segment range camera=" << camera_id
               << " stream=" << stream_id
               << " pool=" << hot_pool.id
               << " path=" << file_path;
    }
    return ok;
}

bool TierStorageManager::syncBackfillTasks() {
    GET_CONFIG(bool, backfill_enabled, Storage::kTierRangeBackfillEnabled);
    if (!backfill_enabled)
        return true;

    GET_CONFIG(int64_t, backfill_days, Storage::kTierRangeBackfillDays);
    if (backfill_days <= 0)
        return true;

    auto device_tuples = collectGenericRtspCameraSources();
    if (device_tuples.empty())
        return false;

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t backfill_start = now - backfill_days * 86400;
    int64_t stable_end = now - 120;
    if (stable_end <= backfill_start)
        return true;

    GET_CONFIG(std::string, record_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    std::string timeline_root = File::absolutePath(app_name, record_path);
    TierBackfillTaskImp task_imp;

    for (const auto &tuple : device_tuples) {
        const auto &camera_id = tuple.device_id;
        TierBackfillTask desired;
        desired.camera_id = camera_id;
        desired.timeline_path = timeline_root + "/" + camera_id;
        desired.window_start = backfill_start;
        desired.cutoff_time = stable_end;
        desired.cursor_time = backfill_start;

        if (task_imp.ensureTask(desired).task_id.empty())
            WarnL << "Cannot ensure tier backfill task camera=" << camera_id;
    }
    return true;
}

bool TierStorageManager::prepareBackfillChunk(const TierBackfillTask &task,
                                              TierBackfillChunk &chunk,
                                              std::string &error) {
    GET_CONFIG(int64_t, chunk_seconds, Storage::kTierRangeBackfillChunkSeconds);
    if (chunk_seconds <= 0)
        chunk_seconds = 86400;

    chunk = TierBackfillChunk();
    chunk.start_time = task.cursor_time;
    chunk.end_time = std::min<int64_t>(task.cursor_time + chunk_seconds,
                                      task.cutoff_time);
    error.clear();
    if (chunk.end_time <= chunk.start_time)
        return true;

    MediaTuple tuple;
    tuple.app = task.camera_id;
    std::vector<TimeBlock> blocks;
    if (!File::fileExist(task.timeline_path)) {
        return true;
    }
    const std::string files = timelineFilesForWindow(task.timeline_path,
                                                     chunk.start_time, chunk.end_time);
    if (files.empty())
        return true;
    try {
        TimeQuery query(tuple, files);
        // A read error must retry the chunk; a partial list would advance the
        // cursor past blocks that were never indexed.
        if (!query.queryBlocks(static_cast<uint64_t>(chunk.start_time),
                               static_cast<uint64_t>(chunk.end_time), blocks, error)) {
            return false;
        }
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    } catch (...) {
        error = "Unknown timeline open error";
        return false;
    }

    // Keep backfilled ranges shaped like recorder-registered ones: split at
    // gaps larger than the merge gap and never across an existing range.
    const int64_t merge_gap_seconds = kSegmentMergeGapSeconds;
    struct PlannedBlock {
        int64_t start_time;
        int64_t end_time;
        int64_t file_size;
    };
    struct PlannedGroup {
        std::string stream_id;
        std::string pool_id;
        std::vector<PlannedBlock> blocks;
    };

    // Existing starts bound new pieces; the window is wide enough to see the
    // next range after any block of this chunk within the same local day.
    std::unordered_map<std::string, std::vector<SegmentTierRange>> existing_by_stream;
    for (const auto &range : SegmentTierRangeImp().queryByCamera(
             task.camera_id, chunk.start_time - 86400, chunk.end_time + 2 * 86400)) {
        existing_by_stream[range.stream_id].push_back(range);
    }

    GET_CONFIG(int64_t, reconcile_min_age, Storage::kTierReconcileMinAgeSeconds);
    GET_CONFIG(bool, reconcile_enabled, Storage::kTierReconcileEnabled);
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int64_t verify_before = now - std::max<int64_t>(0, reconcile_min_age);
    std::unordered_map<std::string, bool> pool_readable;

    std::unordered_set<std::string> seen_segments;
    std::map<std::string, PlannedGroup> groups;
    std::string first_orphan;
    for (const auto &block : blocks) {
        int64_t block_start = static_cast<int64_t>(block.start_time());
        int64_t block_end = block_start + std::max<int64_t>(
            1, static_cast<int64_t>(block.time_len()));
        // A chunk owns blocks by start time, like a range does.
        if (block_start < chunk.start_time || block_start >= chunk.end_time)
            continue;
        ++chunk.scanned_blocks;

        bool already_indexed = false;
        auto existing_it = existing_by_stream.find(block.stream());
        if (existing_it != existing_by_stream.end()) {
            for (const auto &range : existing_it->second) {
                if (range.start_time <= block_start && block_start < range.end_time) {
                    already_indexed = true;
                    break;
                }
            }
        }
        if (already_indexed)
            continue;

        std::string full_path;
        if (!block.file_path().empty())
            full_path = decodeBase64(block.file_path());
        if (full_path.empty() || block.stream().empty()) {
            ++chunk.orphan_blocks;
            if (first_orphan.empty())
                first_orphan = "<block without stream or file path>";
            continue;
        }

        StoragePool hot_pool;
        std::string hot_root;
        if (!resolveHotPoolForPath(full_path, hot_pool, hot_root)) {
            ++chunk.orphan_blocks;
            if (first_orphan.empty())
                first_orphan = full_path;
            continue;
        }

        std::string identity = block.stream() + "\x1f" + full_path;
        if (!seen_segments.insert(identity).second)
            continue;

        // A block whose MP4 is gone (e.g. deleted by the pre-tier storage
        // manager without its timefile rebuild completing) must not become
        // tier metadata; drop it from the timeline once the chunk commits.
        struct stat file_st{};
        if (reconcile_enabled && block_end <= verify_before &&
            ::stat(full_path.c_str(), &file_st) != 0) {
            if (errno != ENOENT) {
                error = std::string("Cannot stat timeline segment ") + full_path + ": " + strerror(errno);
                return false;
            }
            auto readable_it = pool_readable.find(hot_pool.id);
            if (readable_it == pool_readable.end())
                readable_it = pool_readable.emplace(hot_pool.id, isFilePoolVerifiablyReadable(hot_pool)).first;
            if (!readable_it->second) {
                error = "HOT pool is not verifiably readable, cannot check timeline segment: " + hot_pool.id;
                return false;
            }
            std::string segment_path;
            if (deriveSegmentPathFromFullPath(full_path, task.camera_id, block.stream(), segment_path)) {
                TierBackfillMissingBlock missing;
                missing.stream_id = block.stream();
                missing.start_time = block_start;
                missing.segment_path = segment_path;
                chunk.missing_blocks.push_back(missing);
                continue;
            }
        }

        uint64_t raw_size = block.file_size();
        int64_t file_size = raw_size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::max()
            : static_cast<int64_t>(raw_size);
        auto &group = groups[block.stream() + "\x1f" + hot_pool.id];
        group.stream_id = block.stream();
        group.pool_id = hot_pool.id;
        PlannedBlock planned;
        planned.start_time = block_start;
        planned.end_time = block_end;
        planned.file_size = std::max<int64_t>(0, file_size);
        group.blocks.push_back(planned);
        ++chunk.registered_blocks;
        chunk.indexed_bytes += planned.file_size;
    }

    const std::string hot_tier = tierTypeToString(HotTier);
    const std::string available = segmentStatusToString(SegmentStatus::AVAILABLE);
    for (auto &kv : groups) {
        auto &group = kv.second;
        std::sort(group.blocks.begin(), group.blocks.end(),
                  [](const PlannedBlock &x, const PlannedBlock &y) {
                      return x.start_time < y.start_time;
                  });
        std::vector<int64_t> existing_starts;
        auto existing_it = existing_by_stream.find(group.stream_id);
        if (existing_it != existing_by_stream.end()) {
            for (const auto &range : existing_it->second)
                existing_starts.push_back(range.start_time);
        }

        std::sort(existing_starts.begin(), existing_starts.end());
        auto existingStartIn = [&](int64_t after, int64_t up_to) {
            auto it = std::upper_bound(existing_starts.begin(), existing_starts.end(), after);
            return it != existing_starts.end() && *it <= up_to;
        };

        bool open = false;
        int64_t piece_day = 0;
        SegmentTierRange piece;
        auto closePiece = [&]() {
            if (!open)
                return;
            auto next = std::upper_bound(existing_starts.begin(), existing_starts.end(),
                                         piece.start_time);
            if (next != existing_starts.end() && *next < piece.end_time)
                piece.end_time = *next;
            chunk.ranges.push_back(piece);
            open = false;
        };
        for (const auto &planned : group.blocks) {
            const int64_t day = static_cast<int64_t>(
                StampUtils::getStartOfDay(static_cast<uint64_t>(planned.start_time)));
            if (open && (day != piece_day ||
                         planned.start_time - piece.end_time > merge_gap_seconds ||
                         existingStartIn(piece.start_time, planned.start_time))) {
                closePiece();
            }
            if (!open) {
                piece = SegmentTierRange();
                piece.range_id = StrUUID::make_guid(8, "rng");
                piece.camera_id = task.camera_id;
                piece.stream_id = group.stream_id;
                piece.tier = hot_tier;
                piece.pool_id = group.pool_id;
                piece.status = available;
                piece.start_time = planned.start_time;
                piece.end_time = planned.end_time;
                piece.segment_count = 1;
                piece.size_bytes = planned.file_size;
                piece.created_at = now;
                piece.updated_at = now;
                piece_day = day;
                open = true;
                continue;
            }
            piece.end_time = std::max(piece.end_time, planned.end_time);
            ++piece.segment_count;
            piece.size_bytes += planned.file_size;
        }
        closePiece();
    }

    if (chunk.orphan_blocks > 0) {
        // Orphans cannot be tiered (no HOT pool owns their path), but they must
        // not stall the rest of the camera's history behind them.
        WarnL << "Tier backfill skipped " << chunk.orphan_blocks
              << " orphan block(s) camera=" << task.camera_id
              << " chunk=" << chunk.start_time << "-" << chunk.end_time
              << " first=" << first_orphan;
        StorageAlert alert;
        alert.id = StrUUID::make_guid(8, "alert");
        alert.level = "WARNING";
        alert.type = "TIER_BACKFILL_ORPHAN";
        alert.message = StrPrinter << "Backfill skipped " << chunk.orphan_blocks
                                   << " block(s) outside any HOT pool, camera="
                                   << task.camera_id << ", chunk="
                                   << chunk.start_time << "-" << chunk.end_time
                                   << ", first=" << first_orphan;
        alert.created_at = now;
        StorageAlertImp().add(alert);
    }
    return true;
}

bool TierStorageManager::processBackfillTask(const TierBackfillTask &task) {
    TierBackfillTaskImp task_imp;
    TierBackfillChunk chunk;
    std::string error;
    try {
        if (!prepareBackfillChunk(task, chunk, error)) {
            GET_CONFIG(int, max_attempts, Storage::kTierRangeBackfillMaxAttempts);
            GET_CONFIG(int, retry_base_seconds, Storage::kTierRangeBackfillRetryBaseSeconds);
            task_imp.markFailure(task, error, chunk.scanned_blocks,
                                 chunk.orphan_blocks, max_attempts,
                                 retry_base_seconds);
            WarnL << "Tier backfill retry camera=" << task.camera_id
                  << " cursor=" << task.cursor_time
                  << " error=" << error;
            return false;
        }
        if (!task_imp.commitChunk(task, chunk)) {
            GET_CONFIG(int, max_attempts, Storage::kTierRangeBackfillMaxAttempts);
            GET_CONFIG(int, retry_base_seconds, Storage::kTierRangeBackfillRetryBaseSeconds);
            task_imp.markFailure(task, "Atomic backfill commit failed", 0, 0,
                                 max_attempts, retry_base_seconds);
            return false;
        }
        if (!chunk.missing_blocks.empty()) {
            std::vector<TimeBlockIdentity> identities;
            for (const auto &missing : chunk.missing_blocks) {
                TimeBlockIdentity identity;
                identity.camera_id = task.camera_id;
                identity.stream_id = missing.stream_id;
                identity.start_time = static_cast<uint64_t>(missing.start_time);
                identity.segment_path = missing.segment_path;
                identities.push_back(identity);
            }
            StorageManager::Instance().removeCameraTimeBlocks(task.camera_id, identities);
            WarnL << "Tier backfill dropped " << identities.size()
                  << " timeline block(s) without MP4 camera=" << task.camera_id
                  << " first=" << chunk.missing_blocks.front().segment_path;
            StorageAlert alert;
            alert.id = StrUUID::make_guid(8, "alert");
            alert.level = "WARNING";
            alert.type = "TIER_BACKFILL_MISSING_FILE";
            alert.message = StrPrinter << "Backfill removed " << identities.size()
                                       << " timeline block(s) whose MP4 no longer exists, camera="
                                       << task.camera_id << ", first="
                                       << chunk.missing_blocks.front().segment_path;
            alert.created_at = static_cast<int64_t>(time(nullptr));
            StorageAlertImp().add(alert);
        }
        DebugL << "Tier backfill camera=" << task.camera_id
               << " chunk=" << getTimeStr("%Y-%m-%d %H:%M:%S", chunk.start_time)
               << " - " << getTimeStr("%Y-%m-%d %H:%M:%S", chunk.end_time)
               << " scanned=" << chunk.scanned_blocks
               << " registered=" << chunk.registered_blocks
               << " bytes=" << chunk.indexed_bytes;
        return true;
    } catch (const std::exception &ex) {
        GET_CONFIG(int, max_attempts, Storage::kTierRangeBackfillMaxAttempts);
        GET_CONFIG(int, retry_base_seconds, Storage::kTierRangeBackfillRetryBaseSeconds);
        task_imp.markFailure(task, ex.what(), chunk.scanned_blocks,
                             chunk.orphan_blocks, max_attempts,
                             retry_base_seconds);
        WarnL << "Tier backfill failed camera=" << task.camera_id
              << " cursor=" << task.cursor_time << ": " << ex.what();
    } catch (...) {
        GET_CONFIG(int, max_attempts, Storage::kTierRangeBackfillMaxAttempts);
        GET_CONFIG(int, retry_base_seconds, Storage::kTierRangeBackfillRetryBaseSeconds);
        task_imp.markFailure(task, "Unknown backfill error", chunk.scanned_blocks,
                             chunk.orphan_blocks, max_attempts,
                             retry_base_seconds);
        WarnL << "Tier backfill failed camera=" << task.camera_id
              << " cursor=" << task.cursor_time;
    }
    return false;
}

void TierStorageManager::postCycleTask(const std::function<void()> &task) {
    auto tasks = _cycle_tasks;
    {
        std::lock_guard<std::mutex> lk(tasks->mutex);
        ++tasks->tasks_in_flight;
    }
    // may_sync=false: tests call the cycles from any thread, and nothing may run inline.
    BlockingExecutors::tier()->async([tasks, task]() {
        // `task` holds only a weak_ptr; any manager reference it took is gone once it returns.
        task();
        std::lock_guard<std::mutex> lk(tasks->mutex);
        --tasks->tasks_in_flight;
        tasks->completed.notify_all();
    }, false);
}

void TierStorageManager::runBackfillCycle() {
#ifdef ENABLE_MP4
    GET_CONFIG(bool, backfill_enabled, Storage::kTierRangeBackfillEnabled);
    if (!backfill_enabled || _stopping || _backfill_running.exchange(true))
        return;

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    postCycleTask([weak_self]() {
        auto self = weak_self.lock();
        if (!self)
            return;
        struct RunningGuard {
            std::atomic<bool> &running;
            explicit RunningGuard(std::atomic<bool> &value) : running(value) {}
            ~RunningGuard() { running = false; }
        } guard(self->_backfill_running);
        if (self->_stopping)
            return;

        try {
            bool initialized = self->_backfill_tasks_initialized.load();
            if (!initialized || self->_backfill_sync_ticker.elapsedTime() >= 60000) {
                self->_backfill_tasks_initialized = self->syncBackfillTasks();
                self->_backfill_sync_ticker.resetTime();
            }

            GET_CONFIG(int, max_tasks, Storage::kTierRangeBackfillMaxCamerasPerCycle);
            GET_CONFIG(int64_t, work_budget_ms, Storage::kTierRangeBackfillWorkBudgetMs);
            if (max_tasks <= 0)
                max_tasks = 20;
            if (work_budget_ms <= 0)
                work_budget_ms = 2000;

            Ticker budget;
            int processed = 0;
            TierBackfillTaskImp task_imp;
            while (!self->_stopping && processed < max_tasks &&
                   budget.elapsedTime() < static_cast<uint64_t>(work_budget_ms)) {
                auto claimed = task_imp.claimNextDue();
                if (claimed.empty())
                    break;
                self->processBackfillTask(claimed.front());
                ++processed;
            }
            if (processed > 0) {
                InfoL << "Tier backfill dispatch processed_tasks=" << processed
                      << " elapsed=" << format_duration_verbose(budget.elapsedTime());
            }
        } catch (const std::exception &ex) {
            WarnL << "Tier backfill dispatch failed: " << ex.what();
        } catch (...) {
            WarnL << "Tier backfill dispatch failed";
        }
    });
#endif
}

bool TierStorageManager::collectSegmentFilesForRange(const SegmentTierRange &range,
                                                     std::vector<TierRangeSegmentFile> &out,
                                                     std::string &error) {
    out.clear();
    error.clear();
    if (range.camera_id.empty() || range.stream_id.empty() ||
        range.start_time <= 0 || range.end_time <= 0) {
        error = "Invalid tier range identity";
        return false;
    }

    std::vector<TimeBlock> blocks;
    try {
        const std::string files = timelineFilesForWindow(
            cameraTimelineDir(range.camera_id), range.start_time, range.end_time);
        if (files.empty())
            return true;
        mediakit::MediaTuple tuple;
        tuple.app = range.camera_id;
        tuple.stream = range.stream_id;
        TimeQuery query(tuple, files);
        if (!query.queryBlocks(static_cast<uint64_t>(range.start_time),
                               static_cast<uint64_t>(range.end_time), blocks, error)) {
            WarnL << "Cannot read timefile for tier range=" << range.range_id
                  << " camera=" << range.camera_id << ": " << error;
            return false;
        }
    } catch (const std::exception &ex) {
        error = ex.what();
    } catch (...) {
        error = "Unknown timefile open error";
    }
    if (!error.empty()) {
        WarnL << "Cannot open timefile for tier range=" << range.range_id
              << " camera=" << range.camera_id << ": " << error;
        return false;
    }

    StoragePool pool;
    TierFileStorageBase *storage = nullptr;
    if (!range.pool_id.empty()) {
        bool has_pool = false;
        {
            std::lock_guard<std::mutex> lk(_pool_cache_mtx);
            auto it = _pool_cache.find(range.pool_id);
            if (it != _pool_cache.end()) {
                pool = it->second;
                has_pool = true;
            }
        }
        if (has_pool && !poolTypeIsObjectStorage(pool.type)) {
            storage = fileStorageForPoolType(pool.type);
            if (storage && !storage->isRegistered(pool.id) && !registerPoolFileStorage(pool))
                storage = nullptr;
        }
    }

    for (const auto &block : blocks) {
        if (block.app() != range.camera_id || block.stream() != range.stream_id)
            continue;

        int64_t block_start = static_cast<int64_t>(block.start_time());
        int64_t block_end = block_start + std::max<int64_t>(1, static_cast<int64_t>(block.time_len()));
        // A compact range owns segments by their start timestamp in
        // the half-open interval [start_time, end_time). Media files
        // may overlap at their edges and must still belong to one range.
        if (block_start < range.start_time || block_start >= range.end_time)
            continue;

        std::string timefile_path;
        if (!block.file_path().empty())
            timefile_path = decodeBase64(block.file_path());

        std::string segment_path;
        if (!deriveSegmentPathFromFullPath(timefile_path, range.camera_id, range.stream_id, segment_path)) {
            // Skipping it would move or delete the rest while the range still
            // claims this segment, leaving an untracked file behind.
            out.clear();
            error = "Cannot derive segment path from timefile entry: " + timefile_path;
            WarnL << error << " range=" << range.range_id;
            return false;
        }

        TierRangeSegmentFile item;
        item.camera_id = range.camera_id;
        item.stream_id = range.stream_id;
        item.segment_path = segment_path;
        item.storage_key = TierFileStorageBase::makeStorageKey(item.camera_id, item.stream_id, item.segment_path);
        item.start_time = block_start;
        item.end_time = block_end;
        item.file_size = static_cast<int64_t>(block.file_size());
        if (storage)
            item.full_path = storage->resolvePath(pool.id, item.storage_key);
        if (item.full_path.empty())
            item.full_path = timefile_path;

        // An encrypted segment lives under the ".enc" spelling, so both the path and
        // the storage key must describe the variant that is really on disk.
        item.full_path      = resolveLocalVariant(item.full_path);
        item.storage_key    = keyForVariant(item.storage_key, item.full_path);
        item.encrypted      = RecordEncryptionUtils::hasEncSuffix(item.full_path);
        item.tier           = range.tier;
        item.pool_id        = range.pool_id;
        item.origin_pool_id = range.origin_pool_id;

        out.emplace_back(std::move(item));
    }

    std::sort(out.begin(), out.end(), [](const TierRangeSegmentFile &a,
                                         const TierRangeSegmentFile &b) {
        if (a.start_time != b.start_time)
            return a.start_time < b.start_time;
        return a.segment_path < b.segment_path;
    });

    // A successful transcode commits the compact range before the TimeFile is
    // rewritten. Overlay its durable outbox so retention, pressure and quota
    // calculations never temporarily fall back to the old source sizes.
    const std::vector<TierTimefileSizeUpdate> pending_sizes =
        TierTimefileSizeUpdateImp().findPendingByRangeId(range.range_id);
    if (!pending_sizes.empty()) {
        std::map<std::string, int64_t> size_by_identity;
        for (const auto &update : pending_sizes) {
            const std::string key = update.stream_id + "\n" +
                std::to_string(update.start_time) + "\n" + update.segment_path;
            size_by_identity[key] = update.new_size;
        }
        for (auto &segment : out) {
            const std::string key = segment.stream_id + "\n" +
                std::to_string(segment.start_time) + "\n" + segment.segment_path;
            auto it = size_by_identity.find(key);
            if (it != size_by_identity.end())
                segment.file_size = std::max<int64_t>(0, it->second);
        }
    }
    return true;
}

std::vector<TierRangeSegmentFile> TierStorageManager::collectSegmentFilesForRange(const SegmentTierRange &range) {
    std::vector<TierRangeSegmentFile> ret;
    std::string error;
    collectSegmentFilesForRange(range, ret, error);
    return ret;
}

std::string TierStorageManager::getRestoreSegmentPath(const std::string &job_id,
                                                      const std::string &camera_id,
                                                      const std::string &stream_id,
                                                      const std::string &segment_path) const {
    return buildRestoreSegmentPath(job_id, camera_id, stream_id, segment_path);
}

bool TierStorageManager::validateRestoreWindowCache(
        const RestoreJob &job,
        const std::vector<SegmentTierRange> &ranges,
        const std::string &stream_id,
        int64_t start_time,
        int64_t end_time,
        int64_t restore_ttl_seconds,
        int64_t &expires_at) const {
    expires_at = 0;
    if (job.status != jobStatusToString(JobStatus::DONE) ||
        job.camera_id.empty() || job.start_time > start_time || job.end_time < end_time ||
        start_time <= 0 || end_time <= start_time) {
        return false;
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (restore_ttl_seconds >= 0) {
        if (job.updated_at <= 0 ||
            job.updated_at > std::numeric_limits<int64_t>::max() - restore_ttl_seconds)
            return false;
        expires_at = job.updated_at + restore_ttl_seconds;
        if (expires_at <= now)
            return false;
    }

    std::set<std::string> expected_ranges;
    for (const auto &range : ranges) {
        if (range.camera_id != job.camera_id ||
            (!stream_id.empty() && range.stream_id != stream_id) ||
            range.end_time <= start_time || range.start_time >= end_time) {
            continue;
        }
        expected_ranges.insert(range.range_id + "\n" + range.stream_id);
    }
    if (expected_ranges.empty())
        return false;

    std::set<std::string> covered_ranges;
    for (const auto &segment : RestoreJobSegmentImp().queryByJob(job.job_id)) {
        if (segment.camera_id != job.camera_id ||
            (!stream_id.empty() && segment.stream_id != stream_id) ||
            segment.range_end <= start_time || segment.range_start >= end_time) {
            continue;
        }
        const std::string range_key = segment.range_id + "\n" + segment.stream_id;
        if (expected_ranges.find(range_key) == expected_ranges.end())
            continue;
        if (segment.status != jobStatusToString(JobStatus::DONE) ||
            !File::fileExist(resolveRestoreSegmentPath(job, segment))) {
            return false;
        }
        covered_ranges.insert(range_key);
    }
    return !covered_ranges.empty() && covered_ranges == expected_ranges;
}

bool TierStorageManager::getRestoreJobSnapshot(const std::string &job_id,
                                               RestoreJobSnapshot &snapshot) const {
    auto jobs = RestoreJobImp().findByJobId(job_id);
    if (jobs.empty())
        return false;

    snapshot = RestoreJobSnapshot();
    snapshot.job = jobs.front();
    auto children = RestoreJobSegmentImp().queryByJob(job_id);
    snapshot.total_segment_count = static_cast<int>(children.size());
    for (const auto &child : children) {
        if (child.status == jobStatusToString(JobStatus::FAILED)) {
            ++snapshot.failed_segment_count;
            continue;
        }
        if (child.status == jobStatusToString(JobStatus::DONE) &&
            File::fileExist(resolveRestoreSegmentPath(snapshot.job, child))) {
            ++snapshot.restored_segment_count;
            continue;
        }
        ++snapshot.remaining_segment_count;
    }
    if (snapshot.total_segment_count > 0) {
        snapshot.segment_progress_percent = std::max(
            0, std::min(100, snapshot.restored_segment_count * 100 /
                                  snapshot.total_segment_count));
    }

    const std::string &status = snapshot.job.status;
    if (status == "CANCELLED") {
        snapshot.playback_status = "CANCELLED";
    } else if (status == "EXPIRED") {
        snapshot.playback_status = "EXPIRED";
    } else if (status == jobStatusToString(JobStatus::FAILED) ||
               snapshot.failed_segment_count > 0) {
        snapshot.playback_status = "FAILED";
    } else if (status == jobStatusToString(JobStatus::DONE)) {
        bool ttl_valid = true;
        GET_CONFIG(int64_t, restore_ttl_seconds, Storage::kRestoreTTLSeconds);
        if (restore_ttl_seconds >= 0) {
            if (snapshot.job.updated_at <= 0 ||
                snapshot.job.updated_at > std::numeric_limits<int64_t>::max() - restore_ttl_seconds) {
                ttl_valid = false;
            } else {
                snapshot.expires_at = snapshot.job.updated_at + restore_ttl_seconds;
                ttl_valid = snapshot.expires_at > static_cast<int64_t>(time(nullptr));
            }
        }
        if (!ttl_valid) {
            snapshot.playback_status = "EXPIRED";
        } else if (snapshot.total_segment_count > 0 &&
                   snapshot.restored_segment_count == snapshot.total_segment_count) {
            snapshot.playback_status = "READY";
            snapshot.playback_ready = true;
        } else {
            snapshot.playback_status = "NOT_READY";
        }
    } else {
        snapshot.playback_status = "RESTORING";
    }
    return true;
}

bool TierStorageManager::tryRetainRestoreCache(const std::string &job_id) {
    if (job_id.empty())
        return false;
    std::lock_guard<std::mutex> lk(_restore_request_mtx);
    RestoreJobSnapshot snapshot;
    if (!getRestoreJobSnapshot(job_id, snapshot) || !snapshot.playback_ready)
        return false;
    ++_restore_cache_leases[job_id];
    return true;
}

void TierStorageManager::releaseRestoreCache(const std::string &job_id) {
    if (job_id.empty())
        return;
    std::lock_guard<std::mutex> lk(_restore_request_mtx);
    auto it = _restore_cache_leases.find(job_id);
    if (it == _restore_cache_leases.end())
        return;
    if (it->second <= 1) {
        _restore_cache_leases.erase(it);
    } else {
        --it->second;
    }
}

PlaybackPathResolveResult TierStorageManager::resolvePlaybackSegmentPath(const std::string &camera_id,
                                                                         const std::string &stream_id,
                                                                         int64_t segment_start_time,
                                                                         const std::string &timefile_path) {
    PlaybackPathResolveResult ret = resolvePlaybackSegmentPathRaw(camera_id, stream_id,
                                                                  segment_start_time, timefile_path);
    if (!ret.ready || ret.read_path.empty())
        return ret;

    // Every reader goes through here, so decryption is done once, at the point where the
    // path is handed out: an encrypted segment is materialized as a plaintext copy under
    // storage.decrypt_save_path and that copy is what the caller opens.
    std::string err;
    const std::string readable = TierEncryptionManager::Instance().materializeReadablePath(
        camera_id, stream_id, segment_start_time, ret.read_path, &err);
    if (readable.empty()) {
        ret.ready = false;
        ret.message = err.empty() ? "Cannot decrypt segment for reading" : err;
        return ret;
    }
    ret.read_path = readable;
    return ret;
}

PlaybackPathResolveResult TierStorageManager::resolvePlaybackSegmentPathRaw(const std::string &camera_id,
                                                                            const std::string &stream_id,
                                                                            int64_t segment_start_time,
                                                                            const std::string &timefile_path) {
    PlaybackPathResolveResult ret;
    ret.read_path = resolveLocalVariant(timefile_path);
    ret.tier = tierTypeToString(HotTier);

    if (camera_id.empty() || stream_id.empty() || segment_start_time <= 0) {
        ret.ready = !timefile_path.empty();
        ret.fallback = true;
        ret.message = "Invalid segment identity, fallback to timefile path";
        return ret;
    }

    std::string segment_path;
    if (!deriveSegmentPathFromFullPath(timefile_path, camera_id, stream_id, segment_path)) {
        ret.ready = !timefile_path.empty() && File::fileExist(timefile_path);
        ret.fallback = ret.ready;
        ret.message = "Cannot derive segment path, fallback to timefile path";
        return ret;
    }

    SegmentTierRangeImp range_imp;
    auto ranges = range_imp.queryByCamera(camera_id, segment_start_time, segment_start_time + 1);
    SegmentTierRange matched;
    bool found = false;
    bool terminal = false;
    for (const auto &r : ranges) {
        if (r.stream_id != stream_id)
            continue;
        if (r.start_time <= segment_start_time && r.end_time > segment_start_time) {
            matched = r;
            found = true;
            terminal = r.status == segmentStatusToString(SegmentStatus::DELETED) ||
                       r.status == segmentStatusToString(SegmentStatus::EXPIRED) ||
                       r.status == segmentStatusToString(SegmentStatus::MISSING);
            break;
        }
    }
    if (found && terminal) {
        ret.read_path.clear();
        ret.tier = matched.tier;
        ret.pool_id = matched.pool_id;
        ret.range_id = matched.range_id;
        ret.message = "Segment tier range is terminal: " + matched.status;
        return ret;
    }
    if (!found) {
        ret.ready = !timefile_path.empty() && File::fileExist(timefile_path);
        ret.fallback = ret.ready;
        ret.message = "No segment tier range found, fallback to timefile path";
        return ret;
    }

    ret.tier = matched.tier;
    ret.pool_id = matched.pool_id;
    ret.range_id = matched.range_id;

    StoragePool pool;
    bool has_pool = false;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto it = _pool_cache.find(matched.pool_id);
        if (it != _pool_cache.end()) {
            pool = it->second;
            has_pool = true;
        }
    }
    if (!has_pool) {
        ret.ready = !timefile_path.empty() && File::fileExist(timefile_path);
        ret.fallback = ret.ready;
        ret.message = "Segment pool not found, fallback to timefile path";
        return ret;
    }

    if (poolTypeIsObjectStorage(pool.type)) {
        RestoreJobSegment restored_segment;
        restored_segment.camera_id = camera_id;
        restored_segment.stream_id = stream_id;
        restored_segment.segment_path = segment_path;
        GET_CONFIG(int64_t, restore_ttl_seconds, Storage::kRestoreTTLSeconds);
        for (const auto &job : RestoreJobImp().queryCompletedForSegment(
                 camera_id, stream_id, segment_path)) {
            if (restore_ttl_seconds >= 0 &&
                (job.updated_at <= 0 ||
                 job.updated_at > std::numeric_limits<int64_t>::max() - restore_ttl_seconds ||
                 job.updated_at + restore_ttl_seconds <= static_cast<int64_t>(time(nullptr)))) {
                continue;
            }
            auto restore_path = resolveRestoreSegmentPath(job, restored_segment);
            if (!File::fileExist(restore_path))
                continue;
            ret.read_path = restore_path;
            ret.ready = true;
            ret.restore_required = false;
            ret.restore_job_id = job.job_id;
            ret.restore_status = job.status;
            ret.message = "Resolved to restore job cache " + job.job_id;
            return ret;
        }

        PoolHealthInfo health;
        if (!isPoolAvailableForPlayback(pool, &health)) {
            ret.read_path.clear();
            ret.ready = false;
            ret.restore_required = false;
            ret.storage_unavailable = true;
            ret.pool_health_status = health.health_status;
            ret.message = "Object storage pool is unavailable: " + health.health_status;
            return ret;
        }
        ret.read_path.clear();
        ret.ready = false;
        ret.restore_required = true;
        ret.message = "Segment is in object storage and restore cache is not ready";
        return ret;
    }

    PoolHealthInfo health;
    if (!isPoolAvailableForPlayback(pool, &health)) {
        ret.read_path.clear();
        ret.ready = false;
        ret.fallback = false;
        ret.storage_unavailable = true;
        ret.pool_health_status = health.health_status;
        ret.message = "Filesystem storage pool is unavailable: " + health.health_status;
        return ret;
    }

    auto storage = fileStorageForPoolType(pool.type);
    if (!storage) {
        ret.ready = !timefile_path.empty() && File::fileExist(timefile_path);
        ret.fallback = ret.ready;
        ret.message = "Unsupported file storage pool, fallback to timefile path";
        return ret;
    }
    if (!storage->isRegistered(pool.id) && pool.enabled) {
        registerPoolFileStorage(pool);
    }

    std::string storage_key = TierFileStorageBase::makeStorageKey(camera_id, stream_id, segment_path);
    std::string resolved_path = resolveLocalVariant(storage->resolvePath(pool.id, storage_key));
    if (resolved_path.empty() || !File::fileExist(resolved_path)) {
        const bool original_exists = !timefile_path.empty() && File::fileExist(timefile_path);
        ret.read_path = original_exists ? timefile_path : "";
        ret.ready = original_exists;
        ret.fallback = original_exists;
        ret.message = original_exists
            ? "Resolved tier path is missing, using existing timefile path"
            : "Resolved tier path and timefile path are missing";
        return ret;
    }
    ret.read_path = resolved_path;
    ret.ready = true;
    ret.restore_required = false;
    ret.message = "Resolved to file storage pool";
    return ret;
}

PlaybackPathResolveResult TierStorageManager::accessRecordedSegment(
        const std::string &camera_id,
        const std::string &stream_id,
        int64_t segment_start_time,
        const std::string &timefile_path,
        int64_t segment_size,
        bool enqueue_restore,
        const std::string &reason,
        bool retain_restore_cache,
        bool async_enqueue) {
    PlaybackPathResolveResult result;
    if (retain_restore_cache) {
        std::lock_guard<std::mutex> lk(_restore_request_mtx);
        result = resolvePlaybackSegmentPath(camera_id, stream_id, segment_start_time,
                                            timefile_path);
        if (result.ready && !result.restore_job_id.empty()) {
            ++_restore_cache_leases[result.restore_job_id];
        }
    } else {
        result = resolvePlaybackSegmentPath(camera_id, stream_id, segment_start_time,
                                            timefile_path);
    }
    if (!result.restore_required || !enqueue_restore)
        return result;

    if (async_enqueue) {
        requestRestoreSegmentAsync(camera_id, stream_id, segment_start_time, timefile_path, reason,
                                   segment_size);
        result.restore_status = "QUEUED";
        return result;
    }

    auto restore = requestRestoreSegment(camera_id, stream_id, segment_start_time,
                                         timefile_path, reason, segment_size);
    result.restore_job_id = restore.job_id;
    result.restore_status = restore.status;
    if (restore.handled && !restore.message.empty())
        result.message = restore.message;
    return result;
}

namespace {

static std::string normalizedObjectReadMode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (mode != "PRESIGNED" && mode != "HYBRID")
        return "RESTORE";
    return mode;
}

} // namespace

ObjectReadSource TierStorageManager::resolveObjectReadSource(const ObjectReadRequest &request) {
    ObjectReadSource source;

    const PlaybackPathResolveResult resolved = request.retain_restore_cache
        ? accessRecordedSegment(request.camera_id, request.stream_id,
                                request.segment_start_time, request.timefile_path,
                                request.segment_size, false, "OBJECT_READ", true)
        : resolvePlaybackSegmentPath(request.camera_id, request.stream_id,
                                     request.segment_start_time, request.timefile_path);
    source.restore_job_id = resolved.restore_job_id;
    source.message = resolved.message;
    source.storage_unavailable = resolved.storage_unavailable;
    if (resolved.ready) {
        source.kind = ObjectReadSourceKind::LOCAL_PATH;
        source.local_path = resolved.read_path;
        return source;
    }
    if (resolved.storage_unavailable) {
        source.kind = ObjectReadSourceKind::UNAVAILABLE;
        return source;
    }
    if (!resolved.restore_required) {
        source.kind = ObjectReadSourceKind::UNAVAILABLE;
        return source;
    }

    const auto restore_source = [&]() -> ObjectReadSource {
        source.kind = ObjectReadSourceKind::RESTORE_REQUIRED;
        if (!request.enqueue_restore)
            return source;

        const std::string reason = request.restore_reason.empty()
            ? "OBJECT_READ" : request.restore_reason;
        const PlaybackPathResolveResult access = accessRecordedSegment(
            request.camera_id, request.stream_id, request.segment_start_time,
            request.timefile_path, request.segment_size, true, reason,
            request.retain_restore_cache);
        source.restore_job_id = access.restore_job_id;
        source.message = access.message;
        source.storage_unavailable = access.storage_unavailable;
        if (access.ready) {
            source.kind = ObjectReadSourceKind::LOCAL_PATH;
            source.local_path = access.read_path;
            return source;
        }
        if (access.storage_unavailable) {
            source.kind = ObjectReadSourceKind::UNAVAILABLE;
            return source;
        }
        if (access.restore_job_id.empty()) {
            source.kind = ObjectReadSourceKind::UNAVAILABLE;
            source.restore_unavailable = true;
            source.message = access.message.empty()
                ? "Restore job could not be created" : access.message;
            return source;
        }

        RestoreJobSnapshot snapshot;
        if (!getRestoreJobSnapshot(access.restore_job_id, snapshot)) {
            source.kind = ObjectReadSourceKind::UNAVAILABLE;
            source.restore_unavailable = true;
            source.message = "Restore job could not be loaded";
            return source;
        }
        if (snapshot.playback_ready) {
            const PlaybackPathResolveResult ready = accessRecordedSegment(
                request.camera_id, request.stream_id, request.segment_start_time,
                request.timefile_path, request.segment_size, false, reason,
                request.retain_restore_cache);
            if (ready.ready) {
                source.kind = ObjectReadSourceKind::LOCAL_PATH;
                source.local_path = ready.read_path;
                source.restore_job_id = ready.restore_job_id;
                source.message = ready.message;
                return source;
            }
        }
        if (snapshot.playback_status == "FAILED" ||
            snapshot.playback_status == "CANCELLED") {
            source.kind = ObjectReadSourceKind::UNAVAILABLE;
            source.restore_unavailable = true;
            source.message = "Cold-storage restore is " + snapshot.playback_status;
            return source;
        }
        source.progress_percent = static_cast<float>(
            std::max(0, std::min(100, snapshot.segment_progress_percent))) * 0.3f;
        return source;
    };

    GET_CONFIG(std::string, configured_mode, Storage::kObjectReadMode);
    const std::string mode = normalizedObjectReadMode(configured_mode);
    const ObjectReadStrategy strategy = mode == "RESTORE"
        ? ObjectReadStrategy::RESTORE : ObjectReadStrategy::PRESIGNED;
    if (strategy != ObjectReadStrategy::PRESIGNED) {
        return restore_source();
    }

    GET_CONFIG(int64_t, configured_ttl, Storage::kPresignedUrlTTLSeconds);
    const int64_t ttl = std::max<int64_t>(1, std::min<int64_t>(configured_ttl, 7 * 24 * 3600));
    if (request.required_ttl_seconds > ttl) {
        source.message = "Presigned URL lifetime is shorter than the requested operation";
        return restore_source();
    }

    StoragePool pool;
    if (!getPoolById(resolved.pool_id, pool) || !poolTypeIsObjectStorage(pool.type) ||
        !_obj_storage.supportsPresignedRead(pool.id)) {
        source.message = "Object storage provider does not support presigned reads";
        return restore_source();
    }

    std::string segment_path;
    if (!deriveSegmentPathFromFullPath(request.timefile_path, request.camera_id,
                                       request.stream_id, segment_path)) {
        source.message = "Cannot derive object key for direct read";
        return restore_source();
    }
    const std::string object_key = TierObjectStorage::makeS3Key(
        pool.base_path.value_or(""), request.camera_id, request.stream_id,
        segment_path);
    // A segment uploaded while encrypted sits under "<key>.enc", which ffmpeg cannot decrypt,
    // so it has to go through restore. Only the plaintext object may be handed out as a URL.
    if (s_encryption_active.load(std::memory_order_relaxed)) {
        const std::string enc_key = object_key + RecordEncryptionUtils::encryptedFileSuffix();
        if (_obj_storage.headSegment(pool.id, enc_key) == TierObjectPresence::PRESENT) {
            source.message = "Encrypted segment requires restore";
            return restore_source();
        }
        if (_obj_storage.headSegment(pool.id, object_key) != TierObjectPresence::PRESENT) {
            source.message = "Object for direct read was not found";
            return restore_source();
        }
    }
    std::string error;
    if (!_obj_storage.createPresignedGetUrl(pool.id, object_key,
                                            static_cast<uint32_t>(ttl),
                                            source.url, error)) {
        source.message = error;
        return restore_source();
    }
    source.kind = ObjectReadSourceKind::PRESIGNED_URL;
    source.message = "Resolved to a temporary object read source";
    return source;
}

ObjectReadPlan TierStorageManager::planObjectReadWindow(
        const std::string &camera_id,
        const std::string &stream_id,
        int64_t start_time,
        int64_t end_time,
        ObjectReadPurpose purpose) {
    ObjectReadPlan plan;
    if (camera_id.empty() || start_time <= 0 || end_time <= start_time) {
        plan.strategy = ObjectReadStrategy::RESTORE;
        return plan;
    }

    std::set<std::string> identities;
    std::set<std::string> pool_ids;
    SegmentTierRangeImp range_imp;
    for (const auto &range : range_imp.queryByCamera(camera_id, start_time, end_time)) {
        if ((!stream_id.empty() && range.stream_id != stream_id) ||
            range.tier != tierTypeToString(ColdTier) ||
            range.status != segmentStatusToString(SegmentStatus::AVAILABLE) ||
            range.pool_id.empty()) {
            continue;
        }
        for (const auto &file : collectSegmentFilesForRange(range)) {
            if (file.end_time <= start_time || file.start_time >= end_time)
                continue;
            if (!identities.insert(file.stream_id + "\n" + file.segment_path).second)
                continue;
            ++plan.segment_count;
            plan.total_bytes += std::max<int64_t>(0, file.file_size);
            pool_ids.insert(range.pool_id);
        }
    }
    if (plan.segment_count == 0) {
        plan.strategy = ObjectReadStrategy::LOCAL;
        return plan;
    }

    GET_CONFIG(std::string, configured_mode, Storage::kObjectReadMode);
    const std::string mode = normalizedObjectReadMode(configured_mode);
    const ObjectReadStrategy strategy = mode == "RESTORE"
        ? ObjectReadStrategy::RESTORE : ObjectReadStrategy::PRESIGNED;
    if (strategy == ObjectReadStrategy::RESTORE) {
        plan.strategy = ObjectReadStrategy::RESTORE;
        return plan;
    }

    GET_CONFIG(int64_t, configured_ttl, Storage::kPresignedUrlTTLSeconds);
    const int64_t required_ttl = requiredObjectReadTtlSeconds(start_time, end_time, purpose);
    if (purpose == ObjectReadPurpose::EXTRACT && configured_ttl < required_ttl) {
        plan.strategy = ObjectReadStrategy::RESTORE;
        return plan;
    }
    if (mode == "HYBRID" && purpose == ObjectReadPurpose::EXTRACT) {
        GET_CONFIG(int64_t, max_segments, Storage::kPresignedExtractMaxSegments);
        GET_CONFIG(int64_t, max_bytes, Storage::kPresignedExtractMaxBytes);
        if (max_segments <= 0 || max_bytes <= 0 ||
            plan.segment_count > static_cast<size_t>(max_segments) ||
            plan.total_bytes > max_bytes) {
            plan.strategy = ObjectReadStrategy::RESTORE;
            return plan;
        }
    }

    for (const auto &pool_id : pool_ids) {
        StoragePool pool;
        PoolHealthInfo health;
        if (!getPoolById(pool_id, pool) ||
            !isPoolAvailableForPlayback(pool, &health) ||
            !_obj_storage.supportsPresignedRead(pool_id)) {
            plan.strategy = ObjectReadStrategy::RESTORE;
            return plan;
        }
    }
    plan.strategy = ObjectReadStrategy::PRESIGNED;
    return plan;
}

static std::vector<TierRangeSegmentFile> dropSegmentsNotReadyToMove(
        TierStorageManager *self, const TieringJob &job,
        std::vector<TierRangeSegmentFile> segments) {
    std::vector<TierRangeSegmentFile> ready;
    ready.reserve(segments.size());
    size_t busy = 0, pending = 0;
    for (auto &sf : segments) {
        if (self->isSegmentFileBusy(sf.full_path)) {
            // Being rewritten right now. Copying it would capture a partial file.
            ++busy;
            continue;
        }
        if (TierEncryptionManager::Instance().isSegmentPendingEncryption(
                sf.origin_pool_id.empty() && sf.tier == tierTypeToString(HotTier) ? sf.pool_id : sf.origin_pool_id,
                sf.full_path, sf.start_time)) {
            // Still plaintext although a key era covers it. Moving it now would
            // put an unencrypted recording on WARM/COLD storage.
            ++pending;
            continue;
        }
        ready.emplace_back(std::move(sf));
    }
    if (busy || pending) {
        InfoL << "Tiering job " << job.job_id << " deferring " << (busy + pending)
              << " segment(s) (" << busy << " in use, " << pending
              << " awaiting encryption); they move on a later cycle";
    }
    return ready;
}

bool TierStorageManager::collectSegmentFilesForJob(const TieringJob &job,
                                                   SegmentTierRange &out_range,
                                                   std::vector<TierRangeSegmentFile> &out,
                                                   std::string &error) {
    out.clear();
    error.clear();
    SegmentTierRangeImp range_imp;
    if (!job.range_id.empty()) {
        auto ranges = range_imp.findByRangeIds({job.range_id});
        if (ranges.empty()) {
            error = "Tier range no longer exists: " + job.range_id;
            return false;
        }
        out_range = ranges.front();
        if (out_range.camera_id != job.camera_id ||
            out_range.tier != job.source_tier ||
            out_range.pool_id != job.source_pool_id ||
            out_range.status != segmentStatusToString(SegmentStatus::AVAILABLE) ||
            out_range.start_time != job.segment_start_time ||
            out_range.end_time != job.segment_end_time ||
            (job.bytes_total > 0 && out_range.size_bytes != job.bytes_total)) {
            error = "STALE_RANGE_SNAPSHOT: tier range changed after the job was queued";
            return false;
        }
    } else {
        bool found = false;
        auto ranges = range_imp.queryByCamera(job.camera_id, job.segment_start_time, job.segment_end_time);
        for (const auto &range : ranges) {
            if (range.tier != job.source_tier || range.pool_id != job.source_pool_id)
                continue;
            if (range.status != segmentStatusToString(SegmentStatus::AVAILABLE))
                continue;
            if (!job.stream_id.empty() && range.stream_id != job.stream_id)
                continue;
            if (range.start_time < job.segment_start_time || range.end_time > job.segment_end_time)
                continue;
            out_range = range;
            found = true;
            break;
        }
        if (!found) {
            error = "No matching tier range for legacy job window";
            return false;
        }
    }
    if (!collectSegmentFilesForRange(out_range, out, error))
        return false;
    if (out.empty()) {
        error = "No source segments found from tier ranges/timefile";
        return false;
    }
    const size_t collected = out.size();
    out = dropSegmentsNotReadyToMove(this, job, std::move(out));
    if (out.empty()) {
        // The range stays AVAILABLE and only PENDING/RUNNING jobs block a new one, so the
        // next maintenance cycle queues a fresh job for these segments.
        error = "All " + std::to_string(collected) +
                " source segment(s) are in use or awaiting encryption; a later cycle retries them";
        return false;
    }
    return true;
}

bool TierStorageManager::splitRangeForTieringThreshold(const SegmentTierRange &range,
                                                       int64_t move_threshold,
                                                       SegmentTierRange &out_move_range) {
    out_move_range = SegmentTierRange();
    if (range.range_id.empty() || range.pool_id.empty())
        return false;
    if (range.start_time >= move_threshold)
        return false;

    std::vector<TierRangeSegmentFile> segments;
    std::string collect_error;
    if (!collectSegmentFilesForRange(range, segments, collect_error))
        return false;
    if (segments.empty()) {
        WarnL << "Cannot split tier range without timefile segments, range=" << range.range_id
              << " camera=" << range.camera_id;
        return false;
    }

    std::sort(segments.begin(), segments.end(), [](const TierRangeSegmentFile &a,
                                                   const TierRangeSegmentFile &b) {
        return a.start_time < b.start_time;
    });

    size_t move_end = 0;
    while (move_end < segments.size()) {
        const int64_t group_start = segments[move_end].start_time;
        size_t group_end = move_end;
        bool group_eligible = true;
        while (group_end < segments.size() && segments[group_end].start_time == group_start) {
            if (segments[group_end].end_time > move_threshold)
                group_eligible = false;
            ++group_end;
        }
        if (!group_eligible)
            break;
        move_end = group_end;
    }

    int64_t move_count = 0;
    int64_t move_size = 0;
    int64_t remain_count = 0;
    int64_t remain_size = 0;
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto &seg = segments[i];
        if (i < move_end) {
            ++move_count;
            move_size += std::max<int64_t>(0, seg.file_size);
        } else {
            ++remain_count;
            remain_size += std::max<int64_t>(0, seg.file_size);
        }
    }

    int64_t now = static_cast<int64_t>(time(nullptr));
    SegmentTierRange corrected_range = range;
    corrected_range.segment_count = move_count + remain_count;
    corrected_range.size_bytes = move_size + remain_size;
    corrected_range.updated_at = now;

    SegmentTierRangeImp range_imp;
    if (move_end == 0) {
        if ((corrected_range.segment_count != range.segment_count ||
             corrected_range.size_bytes != range.size_bytes) &&
            !range_imp.replaceRangeWithPiecesIfUnchanged(range, {corrected_range})) {
            WarnL << "Failed to refresh tier range stats, range=" << range.range_id;
        }
        return false;
    }
    if (move_end == segments.size()) {
        if (corrected_range.segment_count != range.segment_count ||
            corrected_range.size_bytes != range.size_bytes) {
            if (!range_imp.replaceRangeWithPiecesIfUnchanged(range, {corrected_range})) {
                WarnL << "Failed to refresh movable tier range stats, range=" << range.range_id;
                return false;
            }
        }
        out_move_range = corrected_range;
        return true;
    }

    const int64_t split_time = segments[move_end].start_time;
    if (split_time <= range.start_time || split_time >= range.end_time)
        return false;

    SegmentTierRange move_piece = corrected_range;
    SegmentTierRange remain_piece = corrected_range;
    move_piece.end_time = split_time;
    move_piece.segment_count = move_count;
    move_piece.size_bytes = move_size;
    move_piece.updated_at = now;
    remain_piece.range_id = StrUUID::make_guid(8, "rng");
    remain_piece.start_time = split_time;
    remain_piece.segment_count = remain_count;
    remain_piece.size_bytes = remain_size;
    remain_piece.created_at = now;
    remain_piece.updated_at = now;
    if (move_piece.segment_count <= 0 || remain_piece.segment_count <= 0) {
        WarnL << "Cannot refresh split range stats, range=" << range.range_id
              << " camera=" << range.camera_id
              << " split_time=" << split_time;
        return false;
    }

    if (!range_imp.replaceRangeWithPiecesIfUnchanged(range, {move_piece, remain_piece})) {
        WarnL << "Failed to split tier range for scheduled move, range=" << range.range_id
              << " camera=" << range.camera_id
              << " split_time=" << split_time;
        return false;
    }

    DebugL << "Split tier range " << range.range_id
           << " camera=" << range.camera_id
           << " move_window=" << move_piece.start_time << "-" << move_piece.end_time
           << " remain_window=" << remain_piece.start_time << "-" << remain_piece.end_time;
    out_move_range = move_piece;
    return true;
}

// ===================================================================
// Section 1 — Storage Pool APIs
// ===================================================================
std::string TierStorageManager::createPool(StoragePool pool) {
#ifndef ENABLE_AWS_SDK
    if (poolTypeIsObjectStorage(pool.type)) {
        WarnL << "Object storage pool requires ENABLE_AWS_SDK";
        return "";
    }
#endif
    if (pool.id.empty()) {
        pool.id = StrUUID::make_guid(8, "pool");
        DebugL << "Generated new pool id: " << pool.id;
    }
    pool.created_at = static_cast<int64_t>(time(nullptr));
    pool.updated_at = pool.created_at;

    StoragePoolImp imp;
    if (!imp.add(pool)) {
        WarnL << "Failed to create storage pool: " << pool.name;
        return "";
    }

    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        _pool_cache[pool.id] = pool;
    }
    {
        std::lock_guard<std::mutex> lk(_pool_health_mtx);
        auto &state = _pool_health_cache[pool.id];
        state = PoolHealthRuntimeState();
        state.info.pool_id = pool.id;
        state.info.health_status = healthToString(StorageHealth::UNKNOWN);
    }

    if (pool.enabled) {
        if (poolTypeIsObjectStorage(pool.type))
            registerPoolObjStorage(pool);
        else
            registerPoolFileStorage(pool);
    }

    if (_health_timer)
        runPoolHealthCycle();

    InfoL << "Created storage pool: " << pool.id << " (" << pool.name << ")";
    return pool.id;
}

bool TierStorageManager::updatePool(const StoragePool &pool) {
#ifndef ENABLE_AWS_SDK
    if (poolTypeIsObjectStorage(pool.type)) {
        WarnL << "Object storage pool requires ENABLE_AWS_SDK";
        return false;
    }
#endif
    StoragePool updated = pool;
    updated.updated_at = static_cast<int64_t>(time(nullptr));

    StoragePoolImp imp;
    if (!imp.update(updated)) {
        WarnL << "Failed to update storage pool: " << pool.id;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto it = _pool_cache.find(pool.id);
        if (it != _pool_cache.end()) {
            // preserve secret if caller didn't supply one
            if (!pool.secret_key_enc.has_value())
                updated.secret_key_enc = it->second.secret_key_enc;
            it->second = updated;
        }
    }
    {
        std::lock_guard<std::mutex> lk(_pool_health_mtx);
        auto &state = _pool_health_cache[updated.id];
        state = PoolHealthRuntimeState();
        state.info.pool_id = updated.id;
        state.info.health_status = healthToString(StorageHealth::UNKNOWN);
    }

    // Re-register with the matching storage helper (credentials/path may have changed)
    if (updated.enabled) {
        if (poolTypeIsObjectStorage(updated.type)) {
            unregisterPoolFileStorage(updated.id);
            registerPoolObjStorage(updated);
        } else {
            unregisterPoolObjStorage(updated.id);
            registerPoolFileStorage(updated);
        }
    } else {
        unregisterPoolObjStorage(updated.id);
        unregisterPoolFileStorage(updated.id);
    }
    if (_health_timer)
        runPoolHealthCycle();
    return true;
}

bool TierStorageManager::deletePool(const std::string &pool_id, int &out_ref_count, bool &out_is_default_hot_pool) {
    out_is_default_hot_pool = (pool_id == getSystemDefaultHotPoolId());
    if (out_is_default_hot_pool) {
        WarnL << "Cannot delete default HOT pool: " << pool_id;
        out_ref_count = 0;
        return false;
    }
    StoragePoolImp imp;
    out_ref_count = imp.countPolicyReferences(pool_id);
    if (out_ref_count > 0) {
        WarnL << "Cannot delete pool " << pool_id << ": referenced by " << out_ref_count << " policies";
        return false;
    }
    SegmentTierRangeImp range_imp;
    int range_ref_count = range_imp.countActiveByPool(pool_id);
    if (range_ref_count > 0) {
        out_ref_count = range_ref_count;
        WarnL << "Cannot delete pool " << pool_id << ": referenced by "
              << range_ref_count << " active segment tier ranges";
        return false;
    }
    if (!imp.remove(pool_id)) {
        WarnL << "Failed to remove storage pool: " << pool_id;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        _pool_cache.erase(pool_id);
    }
    {
        std::lock_guard<std::mutex> lk(_pool_health_mtx);
        _pool_health_cache.erase(pool_id);
    }
    unregisterPoolObjStorage(pool_id);
    unregisterPoolFileStorage(pool_id);
    _throughput.removePool(pool_id);
    _hot_forecast.remove(pool_id);
    return true;
}

std::vector<StoragePool> TierStorageManager::listPools(const std::string &tier,
                                                        const std::string &type,
                                                        const std::string &keyword) {
    StoragePoolImp imp;
    auto pools = imp.queryAll(tier, type, keyword);
    for (auto &p : pools) {
        mergePoolRuntimeStats(p);
    }
    return pools;
}

std::vector<StoragePool> TierStorageManager::getPool(const std::string &pool_id) {
    StoragePoolImp imp;
    auto pools = imp.findByPoolId(pool_id);
    for (auto &p : pools) mergePoolRuntimeStats(p);
    return pools;
}

bool TierStorageManager::testPoolConnection(const StoragePool &pool, std::string &out_message, int &out_latency_ms) {
    if (poolTypeIsObjectStorage(pool.type)) {
        if (!pool.endpoint.has_value() || !pool.bucket.has_value() ||
            !pool.access_key.has_value() || !pool.secret_key_enc.has_value()) {
            out_message = "Missing endpoint/bucket/credentials for object storage test";
            return false;
        }
        return _obj_storage.testConnectionParams(
            pool.endpoint.value(),
            pool.bucket.value(),
            pool.access_key.value(),
            decryptPoolSecret(pool.secret_key_enc.value()),
            out_message,
            out_latency_ms);
    }

    std::string path = configuredPoolDataRoot(pool);
    if (path.empty()) {
        out_message = "No mount_path or network_path configured";
        return false;
    }
    auto storage = fileStorageForPoolType(pool.type);
    if (!storage) {
        out_message = "Unsupported file storage type: " + pool.type;
        return false;
    }
    return storage->testConnectionParams(path, out_message, out_latency_ms);
}

std::vector<StorageMountCandidate> TierStorageManager::getAvailableMountPoints(const string &include_types) {
    auto parsed_types = split(include_types, ",");
    auto isIncluded = [&](const std::string &type) {
        if (include_types.empty()) return true;
        for (const auto &inc : parsed_types) {
            if (type == inc) return true;
        }
        return false;
    };

    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    auto local_pools = listPools("", poolTypeToString(StoragePoolType::LOCAL_DISK));
    auto nas_pools = listPools("", poolTypeToString(StoragePoolType::NAS));
    
    std::vector<StorageMountCandidate> ret;
    GET_CONFIG(string, db_path, Database::kDbSavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    auto db_mount = GlobalMonitor::Instance().findMountPoint(File::absolutePath("", db_path));
    auto log_pattern = "/log";
    auto conf_pattern = "/conf";

    for (const auto &hdd : hdd_usage) {
        if (hdd.mount_point == db_mount) {
            DebugL << "Skipping mount point " << hdd.mount_point << " because it is used for database storage";
            continue;
        }
        if (hdd.mount_point.find(log_pattern) != std::string::npos) {
            DebugL << "Skipping mount point " << hdd.mount_point << " because it is used for log storage";
            continue;
        }
        if (hdd.mount_point.find(conf_pattern) != std::string::npos) {
            DebugL << "Skipping mount point " << hdd.mount_point << " because it is used for config storage";
            continue;
        }

        StoragePoolType pool_type = hdd.isNetworkFileSystem() ? StoragePoolType::NAS : StoragePoolType::LOCAL_DISK;
        auto storage = fileStorageForPoolType(poolTypeToString(pool_type));
        if (!storage) {
            continue;
        }
        if (!isIncluded(poolTypeToString(pool_type))) {
            WarnL << "Mount point " << hdd.mount_point << " is not included in the requested storage types: " << include_types;
            continue; // skip if the caller explicitly included this type
        }

        const auto candidate = StorageMountCandidate::fromDiskPartition(
            hdd, poolTypeToString(pool_type), app_name);
        string message;
        int latency_ms = 0;
        // Probe the existing mount itself. Probing candidate.data_path here
        // would create the application directory as a side effect of a GET.
        if (!storage->testConnectionParams(candidate.actual_mount_path, message, latency_ms)) {
            WarnL << "Mount point " << hdd.mount_point << " is not writable, error: " << message;
            continue;
        }
        DebugL << "Mount point " << hdd.mount_point << " is writable, latency: " << latency_ms << " ms";

        bool already_used = false;
        auto poolUsesMount = [&](const StoragePool &pool) {
            const std::string pool_root = resolveFilePoolMountRoot(pool);
            if (pool_root.empty())
                return false;
            if (pool_root == candidate.mount_path)
                return true;
            const std::string pool_data_root = resolveFilePoolDataRoot(pool, app_name);
            return GlobalMonitor::Instance().findMountPoint(pool_data_root) == candidate.actual_mount_path;
        };
        for (const auto &pool : local_pools) {
            if (poolUsesMount(pool)) {
                already_used = true;
                break;
            }
        }
        if (!already_used) {
            for (const auto &pool : nas_pools) {
                if (poolUsesMount(pool)) {
                    already_used = true;
                    break;
                }
            }
        }
        if (!already_used) {
            DebugL << "Available mount point: " << hdd.mount_point << " type=" << poolTypeToString(pool_type) << " fs=" << hdd.filesystem_type;
            ret.push_back(candidate);
        }
    }
    return ret;
}

// ===================================================================
// Section 2 — Storage Policy APIs
// ===================================================================
std::string TierStorageManager::createPolicy(StoragePolicy policy) {
    std::string validation_error;
    if (!validatePolicyForExecution(policy, validation_error)) {
        WarnL << "Reject unsupported storage policy: " << validation_error;
        return "";
    }
    if (policy.id.empty())
        policy.id = StrUUID::make_guid(8, "pol");
    policy.created_at = static_cast<int64_t>(time(nullptr));
    policy.updated_at = policy.created_at;

    if (policy.tiers_json.empty())       policy.tiers_json       = "[]";
    if (policy.delete_policy_json.empty()) policy.delete_policy_json = "{}";
    if (policy.advanced_rules_json.empty()) policy.advanced_rules_json = "{}";

    StoragePolicyImp imp;
    if (!imp.add(policy)) {
        WarnL << "Failed to create storage policy: " << policy.name;
        return "";
    }
    InfoL << "Created storage policy: " << policy.id << " (" << policy.name << ")";
    return policy.id;
}

bool TierStorageManager::updatePolicy(const StoragePolicy &policy) {
    std::string validation_error;
    if (!validatePolicyForExecution(policy, validation_error)) {
        WarnL << "Reject unsupported storage policy update: " << validation_error;
        return false;
    }
    StoragePolicy updated = policy;
    updated.updated_at = static_cast<int64_t>(time(nullptr));

    if (updated.tiers_json.empty())        updated.tiers_json        = "[]";
    if (updated.delete_policy_json.empty()) updated.delete_policy_json = "{}";
    if (updated.advanced_rules_json.empty()) updated.advanced_rules_json = "{}";

    StoragePolicyImp imp;
    bool ok = imp.update(updated);
    if (ok) {
        refreshCamerasRecordRootForPolicy(updated.id);
    }
    return ok;
}

bool TierStorageManager::validatePolicyForExecution(const StoragePolicy &policy,
                                                     std::string &error) const {
    if (!TierStorageBase::validatePolicyTierMoveChain(policy.parsedTiers(), error))
        return false;
    bool has_hot = false;
    for (const auto &tier : policy.parsedTiers()) {
        if (!tier.enabled)
            continue;
        if (tier.tier == tierTypeToString(HotTier))
            has_hot = true;
        if (tierTypeFromString(tier.tier) == TierMax || tier.pool_id.empty()) {
            error = "Enabled tier and pool_id are required";
            return false;
        }
        if (tier.data_mode != tierDataModeToString(TierDataMode::FULL_VIDEO)) {
            error = "Only FULL_VIDEO data_mode is implemented";
            return false;
        }
        if (tier.priority != "NORMAL") {
            error = "Only NORMAL tier priority is implemented";
            return false;
        }
        if (tier.overflow_action == overflowActionToString(OverflowAction::STOP_RECORDING_AND_ALERT)) {
            error = "STOP_RECORDING_AND_ALERT is not implemented";
            return false;
        }
        if (tier.overflow_action == overflowActionToString(OverflowAction::DELETE_OLDEST) &&
            tier.retain_until_days <= 0) {
            error = tier.tier + ".retain_until_days must be greater than 0 for DELETE_OLDEST";
            return false;
        }
        StoragePool pool;
        if (!getPoolById(tier.pool_id, pool) || !pool.enabled || pool.tier != tier.tier) {
            error = "Tier pool is missing, disabled, or belongs to another tier: " + tier.pool_id;
            return false;
        }
#ifndef ENABLE_AWS_SDK
        if (poolTypeIsObjectStorage(pool.type)) {
            error = "Object storage requires ENABLE_AWS_SDK";
            return false;
        }
#endif
    }
    if (!has_hot) {
        error = "An enabled HOT tier is required";
        return false;
    }
    return true;
}

bool TierStorageManager::deletePolicy(const std::string &policy_id, int &out_camera_count, bool &out_is_system_default) {
    out_is_system_default = (policy_id == getSystemDefaultPolicyId());
    if (out_is_system_default) {
        WarnL << "Cannot delete system default storage policy";
        out_camera_count = 0;
        return false;
    }
    StoragePolicyImp imp;
    out_camera_count = imp.countCameraAssignments(policy_id);
    if (out_camera_count > 0) {
        WarnL << "Cannot delete policy " << policy_id << ": used by " << out_camera_count << " cameras";
        return false;
    }
    return imp.remove(policy_id);
}

std::vector<StoragePolicy> TierStorageManager::listPolicies(const std::string &keyword,
                                                              int enabled_filter,
                                                              int page, int size) {
    StoragePolicyImp imp;
    return imp.queryAll(keyword, enabled_filter, page, size);
}

int TierStorageManager::countPolicies(const std::string &keyword, int enabled_filter) {
    StoragePolicyImp imp;
    return imp.countAll(keyword, enabled_filter);
}

std::vector<StoragePolicy> TierStorageManager::getPolicy(const std::string &policy_id) {
    StoragePolicyImp imp;
    return imp.findByPolicyId(policy_id);
}

std::string TierStorageManager::clonePolicy(const std::string &policy_id,
                                             const std::string &new_name) {
    auto policies = getPolicy(policy_id);
    if (policies.empty()) {
        WarnL << "Policy not found for clone: " << policy_id;
        return "";
    }
    auto cloned           = policies.front();
    cloned.id             = StrUUID::make_guid(8, "pol");
    cloned.name           = new_name;
    cloned.created_at     = static_cast<int64_t>(time(nullptr));
    cloned.updated_at     = cloned.created_at;

    std::string cloned_id = createPolicy(cloned);
    if (cloned_id.empty()) {
        WarnL << "Failed to clone policy: " << policy_id;
        return "";
    }
    return cloned_id;
}

// ===================================================================
// Section 3 — Policy Assignment APIs
// ===================================================================
bool TierStorageManager::assignPolicyToCamera(const std::string &camera_id,
                                               const std::string &policy_id,
                                               const std::string &reason) {
    // Verify policy exists
    auto policies = getPolicy(policy_id);
    if (policies.empty()) {
        WarnL << "Policy not found: " << policy_id;
        return false;
    }
    auto default_policies = getPolicy(getSystemDefaultPolicyId());
    if (policy_id != getSystemDefaultPolicyId() && !default_policies.empty() &&
        default_policies.front().allow_camera_override == 0) {
        WarnL << "System default policy does not allow camera overrides";
        return false;
    }

    CameraPolicyAssignment a;
    a.camera_id     = camera_id;
    a.policy_id     = policy_id;
    a.override_reason = reason.empty() ? Optional<std::string>() : Optional<std::string>(reason);
    a.assigned_at   = static_cast<int64_t>(time(nullptr));

    PolicyAssignmentImp imp;
    bool ok = imp.assign(a);
    if (ok) {
        refreshCameraRecordRoot(camera_id);
    }
    return ok;
}

int TierStorageManager::assignPolicyToCameras(const std::vector<std::string> &camera_ids,
                                               const std::string &policy_id,
                                               std::vector<std::string> &out_failed) {
    int success = 0;
    for (const auto &id : camera_ids) {
        if (assignPolicyToCamera(id, policy_id))
            ++success;
        else
            out_failed.push_back(id);
    }
    return success;
}

bool TierStorageManager::removeCameraOverride(const std::string &camera_id) {
    PolicyAssignmentImp imp;
    bool ok = imp.remove(camera_id);
    if (ok) {
        refreshCameraRecordRoot(camera_id);
    }
    return ok;
}

int TierStorageManager::removeCamerasOverride(const std::vector<std::string> &camera_ids, std::vector<std::string> &out_failed) {
    PolicyAssignmentImp imp;
    int success = 0;
    for (const auto &id : camera_ids) {
        if (removeCameraOverride(id)) {
            ++success;
        } else {
            out_failed.push_back(id);
        }
    }
    return success;
}

EffectivePolicyResult TierStorageManager::getEffectivePolicy(const std::string &camera_id) {
    EffectivePolicyResult result;
    result.camera_id = camera_id;

    // 1. Camera-level override
    PolicyAssignmentImp imp;
    auto assignments = imp.findByCameraId(camera_id);
    if (!assignments.empty()) {
        result.policy_id = assignments.front().policy_id;
        result.source    = policySourceToString(PolicySource::CAMERA);
        auto policies = getPolicy(result.policy_id);
        if (!policies.empty()) {
            result.policy_name           = policies.front().name;
            result.allow_camera_override = (policies.front().allow_camera_override != 0);
        }
        return result;
    }

    // 2. System default fixed policy
    {
        std::lock_guard<std::mutex> lk(_default_policy_mtx);
        if (_default_policy_id.empty())
            _default_policy_id = getSystemDefaultPolicyId();
        result.policy_id = _default_policy_id;
        result.source    = policySourceToString(PolicySource::SYSTEM_DEFAULT);
    }
    auto policies = getPolicy(result.policy_id);
    if (!policies.empty()) {
        result.policy_name           = policies.front().name;
        result.allow_camera_override = (policies.front().allow_camera_override != 0);
        return result;
    }

    // No policy available
    ensureSystemDefaultPolicy();
    result.policy_id = getSystemDefaultPolicyId();
    policies = getPolicy(result.policy_id);
    if (!policies.empty()) {
        result.policy_name           = policies.front().name;
        result.allow_camera_override = (policies.front().allow_camera_override != 0);
    }
    result.source = policySourceToString(PolicySource::SYSTEM_DEFAULT);
    return result;
}

bool TierStorageManager::getPoolById(const std::string &pool_id, StoragePool &out_pool) const {
    out_pool = StoragePool();
    if (pool_id.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto it = _pool_cache.find(pool_id);
        if (it != _pool_cache.end()) {
            out_pool = it->second;
            return true;
        }
    }

    StoragePoolImp pool_imp;
    auto pools = pool_imp.findByPoolId(pool_id);
    if (pools.empty()) {
        return false;
    }
    out_pool = pools.front();
    return true;
}

bool TierStorageManager::getPolicyHotPoolId(const StoragePolicy &policy, std::string &out_pool_id) const {
    out_pool_id.clear();
    auto tiers = policy.parsedTiers();
    std::sort(tiers.begin(), tiers.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
        return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
    });

    for (const auto &tier : tiers) {
        if (!tier.enabled || tier.tier != tierTypeToString(HotTier)) {
            continue;
        }
        out_pool_id = tier.pool_id;
        return !out_pool_id.empty();
    }
    return false;
}

bool TierStorageManager::resolveHotPoolRecordRoot(const StoragePool &pool, std::string &out_record_root) const {
    out_record_root.clear();
    if (pool.id.empty() || pool.enabled == 0 || pool.tier != tierTypeToString(HotTier)) {
        WarnL << "Cannot resolve record root, invalid HOT pool pool_id=" << pool.id;
        return false;
    }
    if (poolTypeIsObjectStorage(pool.type)) {
        WarnL << "Cannot use object storage as MP4 record root pool_id=" << pool.id
              << " type=" << pool.type;
        return false;
    }

    std::string root = resolveFilePoolMountRoot(pool);
    if (root.empty()) {
        WarnL << "Cannot resolve record root, HOT pool has no filesystem path pool_id=" << pool.id;
        return false;
    }

    out_record_root = root;
    return !out_record_root.empty();
}

std::string TierStorageManager::resolvePolicyRecordRoot(const StoragePolicy &policy) const {
    std::string hot_pool_id;
    if (!getPolicyHotPoolId(policy, hot_pool_id)) {
        return "";
    }

    StoragePool pool;
    if (!getPoolById(hot_pool_id, pool)) {
        WarnL << "Cannot resolve record root, HOT pool not found policy=" << policy.id
              << " pool_id=" << hot_pool_id;
        return "";
    }

    std::string record_root;
    if (!resolveHotPoolRecordRoot(pool, record_root)) {
        WarnL << "Cannot resolve record root for policy=" << policy.id
              << " pool_id=" << hot_pool_id;
        return "";
    }
    return record_root;
}

std::string TierStorageManager::resolveCameraRecordRoot(const std::string &camera_id,
                                                        EffectivePolicyResult *out_effective) {
    auto effective = getEffectivePolicy(camera_id);
    if (out_effective) {
        *out_effective = effective;
    }

    auto policies = getPolicy(effective.policy_id);
    if (!policies.empty()) {
        auto record_root = resolvePolicyRecordRoot(policies.front());
        if (!record_root.empty()) {
            return record_root;
        }
    }

    // fallback to system default policy if the effective policy is not valid
    const auto default_policy_id = getSystemDefaultPolicyId();
    auto default_policies = getPolicy(default_policy_id);
    if (!default_policies.empty()) {
        auto record_root = resolvePolicyRecordRoot(default_policies.front());
        if (!record_root.empty()) {
            if (out_effective) {
                out_effective->policy_id = default_policy_id;
                out_effective->policy_name = default_policies.front().name;
                out_effective->source = policySourceToString(PolicySource::SYSTEM_DEFAULT);
                out_effective->allow_camera_override = (default_policies.front().allow_camera_override != 0);
            }
            return record_root;
        }
    }
    // Return the default mp4_save_path if no valid policy or pool is found
    GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
    string default_root = File::absolutePath("", mp4_save_path);
    return default_root;
}

bool TierStorageManager::refreshCameraRecordRoot(const std::string &camera_id) {
    if (camera_id.empty()) {
        return false;
    }

    EffectivePolicyResult effective;
    std::string record_root = resolveCameraRecordRoot(camera_id, &effective);

    bool ok = false;
    DeviceSource::Ptr device_source;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
        if (!device_source && src && src->getDeviceTuple().device_id == camera_id) {
            device_source = src;
        }
    }, GENERIC_RTSP_CAMERA_SCHEMA, "", camera_id);

    if (device_source) {
        device_source->getOwnerPoller()->async([device_source, record_root]() {
            if(!device_source->setRecordRootPath(record_root)) {
                WarnL << "Camera do not support setRecordRootPath, camera=" << device_source->getDeviceTuple().device_id
                      << " record_root=" << record_root;
            }
        });
        ok = true;
    }

    if (!ok) {
        WarnL << "Failed to refresh mp4_save_path for camera=" << camera_id
              << " policy=" << effective.policy_id
              << " root=" << record_root;
    } else {
        InfoL << "Refreshed mp4_save_path for camera=" << camera_id
              << " policy=" << effective.policy_id
              << " root=" << record_root;
    }
    return ok;
}

void TierStorageManager::refreshCamerasRecordRootForPolicy(const std::string &policy_id) {
    if (policy_id.empty()) {
        return;
    }

    if (policy_id == getSystemDefaultPolicyId()) {
        for (const auto &tuple : collectGenericRtspCameraSources()) {
            auto effective = getEffectivePolicy(tuple.device_id);
            if (effective.policy_id == policy_id) {
                refreshCameraRecordRoot(tuple.device_id);
            }
        }
        return;
    }

    PolicyAssignmentImp assignment_imp;
    for (const auto &camera_id : assignment_imp.findCamerasByPolicyId(policy_id)) {
        refreshCameraRecordRoot(camera_id);
    }
}

// ===================================================================
// Section 4 — Camera Storage APIs
// ===================================================================
std::vector<TimelineRange> TierStorageManager::getCameraTimeline(const std::string &camera_id,
                                                                   int64_t start_time,
                                                                   int64_t end_time) {
    SegmentTierRangeImp range_imp;
    auto ranges = range_imp.queryByCamera(camera_id, start_time, end_time);

    std::vector<TimelineRange> result;
    result.reserve(ranges.size());
    for (const auto &r : ranges) {
        TimelineRange tr;
        tr.start = r.start_time;
        tr.end = r.end_time;
        tr.tier = r.tier;
        tr.pool_id = r.pool_id;
        tr.status = r.status.empty() ? segmentStatusToString(SegmentStatus::AVAILABLE) : r.status;
        tr.segment_count = r.segment_count;
        tr.size_bytes = r.size_bytes;
        result.push_back(tr);
    }
    if (!result.empty())
        return result;

    return result;
}

CameraStorageSummary TierStorageManager::getCameraStorageSummary(const std::string &camera_id) {
    CameraStorageSummary summary;
    summary.camera_id = camera_id;

    SegmentTierRangeImp range_imp;
    auto ranges = range_imp.queryByCamera(camera_id);

    std::unordered_map<std::string, TierSummaryInfo> tier_map;
    for (const auto &r : ranges) {
        if (r.status == segmentStatusToString(SegmentStatus::DELETED) ||
            r.status == segmentStatusToString(SegmentStatus::MISSING))
            continue;
        const std::string summary_key = r.tier + "\n" + r.pool_id;
        auto &ti = tier_map[summary_key];
        ti.tier = r.tier;
        ti.pool_id = r.pool_id;
        ti.used_bytes += r.size_bytes;
        ti.segment_count += r.segment_count;
        if (ti.oldest_segment_time == 0 || r.start_time < ti.oldest_segment_time)
            ti.oldest_segment_time = r.start_time;
        if (r.end_time > ti.newest_segment_time)
            ti.newest_segment_time = r.end_time;

        summary.total_used_bytes += r.size_bytes;
        summary.total_segments += r.segment_count;
        if (summary.oldest_time == 0 || r.start_time < summary.oldest_time)
            summary.oldest_time = r.start_time;
        if (r.end_time > summary.newest_time)
            summary.newest_time = r.end_time;
    }

    if (!tier_map.empty()) {
        for (auto &kv : tier_map)
            summary.tiers.push_back(std::move(kv.second));
        return summary;
    }

    return summary;
}

// ===================================================================
// Tiering engine
// ===================================================================
void TierStorageManager::runTieringCycle() {

    if (_stopping || _maintenance_running.exchange(true)) {
        DebugL << "Skip overlapping tier storage maintenance cycle";
        return;
    }

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    postCycleTask([weak_self]() {
        auto self = weak_self.lock();
        if (!self) return;

        struct CycleGuard {
            std::atomic<bool> &running;
            explicit CycleGuard(std::atomic<bool> &flag) : running(flag) {}
            ~CycleGuard() { running = false; }
        } guard(self->_maintenance_running);

        if (self->_stopping)
            return;

        try {
            self->_ticker.resetTime();
            InfoL << "Starting tiering cycle";

        // Refresh pool cache
        {
            StoragePoolImp pool_imp;
            auto all_pools = pool_imp.queryAll();
            std::lock_guard<std::mutex> lk(self->_pool_cache_mtx);
            self->_pool_cache.clear();
            for (const auto &p : all_pools)
                self->_pool_cache[p.id] = p;
        }
        self->_dest_pool_checks.clear();

        self->ensureSystemDefaultPolicy();

        // Resume durable work without blocking the maintenance scan.
        self->dispatchPendingTieringJobs();

        // Gather cameras from compact ranges.
        SegmentTierRangeImp range_imp;
        auto camera_ids = range_imp.findDistinctAvailableCameras();

        std::vector<std::pair<std::string, StoragePolicy>> camera_policies;
        for (const auto &cam_id : camera_ids) {
            try {
                auto eff = self->getEffectivePolicy(cam_id);
                if (!eff.policy_id.empty()) {
                    auto policies = self->getPolicy(eff.policy_id);
                    if (!policies.empty())
                        camera_policies.emplace_back(cam_id, policies.front());
                }
            } catch (const std::exception &e) {
                WarnL << "Error loading policy of camera " << cam_id << ": " << e.what();
            }
        }

        // Before this cycle queues new moves, so the backlog is what the last cycle left behind.
        try {
            self->evaluatePoolThroughput(camera_policies);
        } catch (const std::exception &e) {
            WarnL << "Error evaluating pool throughput: " << e.what();
        }
        try {
            self->evaluateRetentionForecast(camera_policies);
        } catch (const std::exception &e) {
            WarnL << "Error evaluating retention forecast: " << e.what();
        }

        // Early moves are balanced across cameras, so plan them per pool first.
        try {
            self->planBalancedPressureMoves(camera_policies);
        } catch (const std::exception &e) {
            WarnL << "Error planning pressure moves: " << e.what();
        }

        for (const auto &item : camera_policies) {
            const auto &cam_id = item.first;
            const auto &policy = item.second;
            try {
                self->processCameraTiering(cam_id, policy);
                self->processCameraPressureTiering(cam_id, policy);
                self->enforcePolicyDeleteRetention(cam_id, policy);
                self->enforceCameraArchiveRetention(cam_id, policy);
            } catch (const std::exception &e) {
                WarnL << "Error processing camera " << cam_id << ": " << e.what();
            }
        }

            self->enforcePoolPressureRetention();
            self->runRangeFileReconcile();
            self->runObjectRangeReconcile();
            self->processMetadataCleanupJobs();
            self->processTimefileSizeUpdates();
            self->processCommittedSourceCleanup();
            // Gap fill appends past blocks after newer ones; rewrite those files in order so the
            // ranges of the filled segments can be listed (and moved, and expired).
            TimeRecorderManager::Instance().sortUnsortedTimeFiles();
            self->processRestoreJobs();
            self->pruneOldMetrics();
            self->cleanupRestoreTempFiles();
            TierEncryptionManager::Instance().sweepDecryptCache();
            self->cleanupOrphanTranscodeStaging();
            self->cleanupOrphanTemporarySegments();
            self->sweepStaleTimeBlocks();
            self->dispatchPendingTieringJobs();
            InfoL << "Tiering cycle done. Elapsed: " << format_duration_verbose(self->_ticker.elapsedTime());
        } catch (const std::exception &ex) {
            WarnL << "Tier storage maintenance cycle failed: " << ex.what();
        } catch (...) {
            WarnL << "Tier storage maintenance cycle failed with unknown error";
        }
    });
}

void TierStorageManager::processTimefileSizeUpdates() {
    TierTimefileSizeUpdateImp updates;
    const std::vector<TierTimefileSizeUpdate> pending = updates.findPending(1000);
    std::map<std::string, std::vector<TierTimefileSizeUpdate>> by_camera;
    for (const auto &update : pending) by_camera[update.camera_id].push_back(update);
    for (const auto &entry : by_camera) {
        std::vector<TimeBlockSizePatch> patches;
        for (const auto &update : entry.second) {
            TimeBlockSizePatch patch;
            patch.camera_id = update.camera_id;
            patch.stream_id = update.stream_id;
            patch.start_time = static_cast<uint64_t>(update.start_time);
            patch.segment_path = update.segment_path;
            patch.new_size = static_cast<uint64_t>(std::max<int64_t>(0, update.new_size));
            patches.push_back(patch);
        }
        try {
            if (StorageManager::Instance().verifyCameraTimeBlockSizes(entry.first, patches) ==
                patches.size()) {
                for (const auto &update : entry.second) updates.markResult(update, "DONE");
                continue;
            }
            const size_t patched =
                StorageManager::Instance().patchCameraTimeBlockSizes(entry.first, patches);
            const std::string result = patched > 0
                ? "Awaiting TimeFile swap verification"
                : "No matching TimeFile block patched";
            for (const auto &update : entry.second)
                updates.markResult(update, "PENDING", result);
        } catch (const std::exception &ex) {
            for (const auto &update : entry.second)
                updates.markResult(update, "PENDING", ex.what());
        } catch (...) {
            for (const auto &update : entry.second)
                updates.markResult(update, "PENDING", "Unknown TimeFile patch error");
        }
    }
}

void TierStorageManager::processCommittedSourceCleanup() {
    TieringJobSegmentImp segment_imp;
    const std::vector<TieringJobSegment> committed =
        segment_imp.findCommittedForCleanup(1000);
    for (const auto &segment : committed) {
        const std::vector<TieringJob> jobs = TieringJobImp().findByJobId(segment.job_id);
        if (jobs.empty()) continue;
        StoragePool source_pool;
        {
            std::lock_guard<std::mutex> lk(_pool_cache_mtx);
            auto it = _pool_cache.find(jobs.front().source_pool_id);
            if (it == _pool_cache.end()) continue;
            source_pool = it->second;
        }
        if (poolTypeIsObjectStorage(source_pool.type))
            continue;
        TierFileStorageBase *storage = fileStorageForPoolType(source_pool.type);
        if (!storage || (!storage->isRegistered(source_pool.id) &&
                         !registerPoolFileStorage(source_pool)))
            continue;
        if (!storage->segmentExists(source_pool.id, segment.storage_key) ||
            storage->deleteSegment(source_pool.id, segment.storage_key)) {
            segment_imp.markCleaned(segment);
        }
    }
}

void TierStorageManager::cleanupOrphanTranscodeStaging() {
    GET_CONFIG(std::string, root_value, Storage::kTierTranscodeStagingPath);
    GET_CONFIG(int64_t, timeout_seconds, Storage::kTierTranscodeTimeoutSeconds);
    const std::string root = normalizeStoragePath(root_value);
    struct stat root_stat{};
    if (root.empty() || root == "/" || ::lstat(root.c_str(), &root_stat) != 0 ||
        !S_ISDIR(root_stat.st_mode)) {
        return;
    }

    std::unordered_set<std::string> active_job_keys;
    TieringJobImp jobs;
    const std::vector<std::string> active_statuses = {
        jobStatusToString(JobStatus::PENDING), jobStatusToString(JobStatus::RUNNING)};
    for (const auto &status : active_statuses) {
        const std::vector<TieringJob> active = jobs.findByStatus(status);
        for (const auto &job : active) {
            if (job.operation == "TRANSCODE_MOVE")
                active_job_keys.insert(tierTranscodeStagingJobKey(job.job_id));
        }
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int64_t stale_after = std::max<int64_t>(1, timeout_seconds) * 2;
    size_t examined = 0;
    size_t deleted = 0;
    const size_t max_files = 1000;
    File::scanDir(root, [&](const std::string &job_path, bool) {
        if (_stopping || examined >= max_files)
            return false;
        struct stat job_stat{};
        if (::lstat(job_path.c_str(), &job_stat) != 0 || !S_ISDIR(job_stat.st_mode))
            return true;
        const size_t slash = job_path.find_last_of('/');
        const std::string job_key = slash == std::string::npos
            ? job_path : job_path.substr(slash + 1);
        if (active_job_keys.count(job_key) != 0)
            return true;

        File::scanDir(job_path, [&](const std::string &stream_path, bool) {
            if (_stopping || examined >= max_files)
                return false;
            struct stat stream_stat{};
            if (::lstat(stream_path.c_str(), &stream_stat) != 0 ||
                !S_ISDIR(stream_stat.st_mode)) {
                return true;
            }
            File::scanDir(stream_path, [&](const std::string &path, bool) {
                if (_stopping || examined >= max_files)
                    return false;
                ++examined;
                struct stat file_stat{};
                if (::lstat(path.c_str(), &file_stat) != 0 ||
                    !S_ISREG(file_stat.st_mode) || !end_with(path, ".mp4.tmp") ||
                    now - static_cast<int64_t>(file_stat.st_mtime) <= stale_after) {
                    return true;
                }
                if (::unlink(path.c_str()) == 0) {
                    ++deleted;
                } else if (errno != ENOENT) {
                    WarnL << "Cannot remove orphan tier transcode staging file: "
                          << path << ": " << strerror(errno);
                }
                return true;
            }, false, false);
            return true;
        }, false, false);
        return true;
    }, false, false);

    if (deleted > 0) {
        DebugL << "cleanupOrphanTranscodeStaging: examined=" << examined
               << ", deleted=" << deleted << ", root=" << root;
    }
}

void TierStorageManager::processCameraTiering(const std::string &camera_id,
                                               const StoragePolicy &policy) {
    if (policy.enabled == 0)
        return;

    auto tiers = policy.parsedTiers();
    auto rules = policy.parsedAdvancedRules();
    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t min_age_secs = static_cast<int64_t>(rules.min_segment_age_minutes_before_move) * 60;

    // Sort tiers: HOT first, then WARM, then COLD
    std::sort(tiers.begin(), tiers.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
        return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
    });

    SegmentTierRangeImp range_imp;
    GET_CONFIG(int, max_ranges_per_cycle, Storage::kTieringMaxRangesPerCameraCycle);
    if (max_ranges_per_cycle <= 0)
        max_ranges_per_cycle = 64;

    auto next_enabled_tier = [&](size_t index) -> const PolicyTierConfig * {
        for (size_t j = index + 1; j < tiers.size(); ++j) {
            if (tiers[j].enabled)
                return &tiers[j];
        }
        return nullptr;
    };

    for (size_t i = 0; i < tiers.size(); ++i) {
        const auto &src_tier_cfg = tiers[i];
        const auto *dst_tier_cfg = next_enabled_tier(i);
        if (!src_tier_cfg.enabled)
            continue;
        if (src_tier_cfg.pool_id.empty()) {
            WarnL << "Skip tiering config with empty pool_id camera=" << camera_id
                  << " source_tier=" << src_tier_cfg.tier;
            continue;
        }

        // Age threshold: segments whose end_time is before this should move.
        // Compact ranges may still be open because cameras keep recording, so
        // query by start_time and split the old portion at segment boundaries.
        int64_t move_threshold = now - (static_cast<int64_t>(src_tier_cfg.retain_until_days) * 86400);

        // Respect min_segment_age
        if (move_threshold > now - min_age_secs)
            move_threshold = now - min_age_secs;
        move_threshold = alignTierThreshold(move_threshold);
        DebugL << "Processing tiering for camera=" << camera_id
               << " source_tier=" << src_tier_cfg.tier
               << " target_tier=" << (dst_tier_cfg ? dst_tier_cfg->tier : "")
               << " move_threshold=" << getTimeStr("%Y-%m-%d %H:%M:%S", move_threshold);
        // Splitting rewrites ranges; skip it when no move could be queued anyway.
        if (src_tier_cfg.overflow_action == overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER) &&
            dst_tier_cfg && !dst_tier_cfg->pool_id.empty() &&
            isDestinationPoolCritical(dst_tier_cfg->pool_id)) {
            continue;
        }
        auto ranges = range_imp.findByCameraTierStartedBefore(
            camera_id, src_tier_cfg.tier, move_threshold, max_ranges_per_cycle);

        TieringJobImp job_imp;
        for (const auto &range : ranges) {
            if (range.pool_id.empty()) continue;
            // Splitting reads the timefile; skip ranges already queued for transfer.
            if (job_imp.hasActiveJobForRange(range.range_id))
                continue;
            SegmentTierRange move_range;
            if (!splitRangeForTieringThreshold(range, move_threshold, move_range))
                continue;

            if (src_tier_cfg.overflow_action == overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER)) {
                if (!dst_tier_cfg || dst_tier_cfg->pool_id.empty()) {
                    WarnL << "Skip move because target tier is disabled or has empty pool_id camera=" << camera_id
                          << " source_tier=" << src_tier_cfg.tier
                          << " target_tier=" << (dst_tier_cfg ? dst_tier_cfg->tier : "");
                    continue;
                }
                queueTierMoveJob(move_range, src_tier_cfg, *dst_tier_cfg, policy,
                                 TieringJobTrigger::RETENTION);
            } else if (src_tier_cfg.overflow_action == overflowActionToString(OverflowAction::DELETE_OLDEST)) {
                expireRangeBestEffort(move_range, segmentStatusToString(SegmentStatus::DELETED));
            } else if (src_tier_cfg.overflow_action == overflowActionToString(OverflowAction::STOP_RECORDING_AND_ALERT)) {
                WarnL << "Storage policy requests STOP_RECORDING_AND_ALERT camera=" << camera_id
                      << " tier=" << src_tier_cfg.tier;
            }
        }
    }
}

// The policy tier that asks for quality reduction when moving into `pool_id`. False while the
// operational switch or the policy master switch is off: moves then behave as plain copies.
static bool findQualityReductionTier(const StoragePolicy &policy, const std::string &pool_id,
                                     PolicyTierConfig &out) {
    GET_CONFIG(bool, transcode_enabled, Storage::kTierTranscodeEnabled);
    if (!transcode_enabled || !policy.parsedAdvancedRules().enable_tier_quality_reduction)
        return false;
    const std::vector<PolicyTierConfig> tiers = policy.parsedTiers();
    for (const auto &tier : tiers) {
        if (tier.enabled && tier.pool_id == pool_id &&
            tier.tier != tierTypeToString(HotTier) && tier.quality_reduction.enabled) {
            out = tier;
            return true;
        }
    }
    return false;
}

bool TierStorageManager::queueTierMoveJob(const SegmentTierRange &range,
                                          const PolicyTierConfig &src_tier_cfg,
                                          const PolicyTierConfig &dst_tier_cfg,
                                          const StoragePolicy &policy,
                                          TieringJobTrigger trigger,
                                          bool force_copy,
                                          bool shape_protected) {
    PolicyTierConfig quality_tier;
    const bool quality_target =
        findQualityReductionTier(policy, dst_tier_cfg.pool_id, quality_tier);
    if (quality_target && !force_copy &&
        SegmentRetentionOverrideImp().isActive(
            range.range_id, static_cast<int64_t>(time(nullptr))))
        force_copy = true;
    std::string source_pool_id = range.pool_id.empty() ? src_tier_cfg.pool_id : range.pool_id;
    if (source_pool_id.empty() || dst_tier_cfg.pool_id.empty()) {
        WarnL << "Refuse to queue tiering job with empty pool_id camera=" << range.camera_id
              << " source_pool_id=" << source_pool_id
              << " target_pool_id=" << dst_tier_cfg.pool_id;
        return false;
    }
    if (source_pool_id == dst_tier_cfg.pool_id) {
        WarnL << "Refuse to queue tiering job with same source/target pool_id camera=" << range.camera_id
              << " pool_id=" << source_pool_id
              << " range=" << range.range_id;
        return false;
    }

    // Only a quality move needs protected segments in pieces of their own (copied unchanged).
    if (shape_protected && quality_target && !force_copy) {
        const std::vector<ProtectedVideo> protections = ProtectedVideoImp().findOverlapping(
            range.camera_id, range.start_time, range.end_time);
        if (!protections.empty()) {
            std::vector<TierRangeSegmentFile> segments;
            std::string error;
            if (!collectSegmentFilesForRange(range, segments, error) || segments.empty()) {
                WarnL << "Cannot shape tier move around protected segments: " << error;
                return false;
            }
            auto protected_segment = [&protections](int64_t start, int64_t end) {
                for (const auto &video : protections) {
                    if (video.start_time <= end && video.end_time >= start)
                        return true;
                }
                return false;
            };
            const int kNormal = 1;
            const int kProtected = 2;
            std::vector<int> kinds(segments.size(), kNormal);
            for (size_t i = 0; i < segments.size(); ++i) {
                if (protected_segment(segments[i].start_time, segments[i].end_time))
                    kinds[i] = kProtected;
            }
            std::vector<ShapedPiece> shaped;
            if (!shapeRangePieces(range, segments, kinds, protected_segment,
                                  static_cast<int64_t>(time(nullptr)), shaped))
                return false;
            if (shaped.size() > 1 || shaped.front().kind == kProtected) {
                std::vector<SegmentTierRange> pieces;
                for (const auto &piece : shaped) pieces.push_back(piece.range);
                if (!SegmentTierRangeImp().replaceRangeWithPiecesIfUnchanged(range, pieces))
                    return false;
                bool queued = false;
                for (const auto &piece : shaped) {
                    queued = queueTierMoveJob(piece.range, src_tier_cfg, dst_tier_cfg,
                                              policy, trigger,
                                              piece.kind == kProtected, false) || queued;
                }
                return queued;
            }
        }
    }

    TieringJobImp job_imp;
    if (job_imp.hasActiveJobForRange(range.range_id) ||
        job_imp.hasActiveJobForWindow(range.camera_id, range.stream_id,
                                      src_tier_cfg.tier, dst_tier_cfg.tier,
                                      range.start_time, range.end_time)) {
        return false;
    }

    if (isDestinationPoolCritical(dst_tier_cfg.pool_id)) {
        DebugL << "Skip tiering job because the destination pool is critical, pool="
               << dst_tier_cfg.pool_id << " range=" << range.range_id;
        return false;
    }

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t retry_at = 0;
    if (missingSegmentRetryAt(range.range_id, retry_at) && now < retry_at) {
        DebugL << "Defer tiering job for range with missing segments until "
               << getTimeStr("%Y-%m-%d %H:%M:%S", retry_at)
               << " range=" << range.range_id;
        return false;
    }

    TieringJob job;
    const bool pressure = trigger == TieringJobTrigger::PRESSURE;
    job.job_id             = StrUUID::make_guid(8, pressure ? "ptj" : "tj");
    job.camera_id          = range.camera_id;
    job.stream_id          = range.stream_id;
    job.range_id           = range.range_id;
    job.source_tier        = src_tier_cfg.tier;
    job.target_tier        = dst_tier_cfg.tier;
    job.source_pool_id     = source_pool_id;
    job.target_pool_id     = dst_tier_cfg.pool_id;
    job.status             = jobStatusToString(JobStatus::PENDING);
    job.segment_start_time = range.start_time;
    job.segment_end_time   = range.end_time;
    job.bytes_total        = range.size_bytes;
    job.created_at         = now;
    job.updated_at         = now;
    job.job_trigger        = tieringJobTriggerToString(trigger);
    job.policy_id          = policy.id;
    job.policy_updated_at  = policy.updated_at;

    if (quality_target && !force_copy) {
        const std::string &codec = quality_tier.quality_reduction.video_codec;
        if (TierEncoderCapabilityRegistry::Instance().operational(codec)) {
            job.operation = "TRANSCODE_MOVE";
            Json::FastWriter writer;
            job.quality_profile_json = writer.write(quality_tier.quality_reduction.toJson());
            if (!job.quality_profile_json.empty() &&
                job.quality_profile_json[job.quality_profile_json.size() - 1] == '\n')
                job.quality_profile_json.resize(job.quality_profile_json.size() - 1);
        } else if (trigger == TieringJobTrigger::PRESSURE) {
            // A full source pool cannot wait for an encoder: move the originals instead.
            WarnL << "Encoder capability unavailable, pressure move copies without "
                     "quality reduction: " << codec << " range=" << range.range_id;
        } else {
            DebugL << "Defer quality tier move because encoder capability is unavailable: "
                   << codec;
            return false;
        }
    }

    if (!job_imp.add(job))
        return false;

    DebugL << "Queued " << (pressure ? "pressure " : "")
           << "tiering range job " << job.job_id
           << " for camera " << range.camera_id
           << " [" << src_tier_cfg.tier << " -> " << dst_tier_cfg.tier << "]"
           << " range: " << range.start_time << "-" << range.end_time;
    return true;
}

TierStorageManager::DestinationPoolCheck
TierStorageManager::destinationPoolCheck(const std::string &pool_id) {
    auto cached = _dest_pool_checks.find(pool_id);
    if (cached != _dest_pool_checks.end())
        return cached->second;

    DestinationPoolCheck check;
    StoragePool pool;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto it = _pool_cache.find(pool_id);
        if (it != _pool_cache.end()) {
            pool = it->second;
            found = true;
        }
    }
    if (found && pool.enabled) {
        fillPoolRuntimeStats(pool);
        check.usage_pct = pool.usage_pct;
        check.has_capacity = pool.total_bytes > 0;
        check.critical = check.has_capacity &&
            pool.usage_pct >= static_cast<float>(pool.critical_watermark_percent);
    }
    _dest_pool_checks[pool_id] = check;
    return check;
}

bool TierStorageManager::isDestinationPoolCritical(const std::string &pool_id) {
    const DestinationPoolCheck check = destinationPoolCheck(pool_id);
    if (!check.critical)
        return false;
    StoragePool pool;
    getPoolById(pool_id, pool);

    const int64_t now = static_cast<int64_t>(time(nullptr));
    auto &alerted_at = _dest_critical_alerted_at[pool_id];
    if (now - alerted_at >= 3600) {
        alerted_at = now;
        WarnL << "Destination pool " << pool_id << " is above its critical watermark ("
              << check.usage_pct << "%); tier moves into it are paused";
        StorageAlert alert;
        alert.id = StrUUID::make_guid(8, "alert");
        alert.level = "ERROR";
        alert.type = "TIER_DEST_POOL_CRITICAL";
        alert.pool_id = pool_id;
        alert.message = StrPrinter << "Destination pool is above its critical watermark ("
                                   << check.usage_pct << "% >= "
                                   << pool.critical_watermark_percent
                                   << "%); tier moves into it are paused";
        alert.created_at = now;
        StorageAlertImp().add(alert);
    }
    return true;
}

void TierStorageManager::planBalancedPressureMoves(
        const std::vector<std::pair<std::string, StoragePolicy>> &camera_policies) {
    _pressure_move_cutoffs.clear();
    GET_CONFIG(std::string, delete_mode, Storage::kTieringPressureDeleteMode);
    if (strcasecmp(delete_mode.c_str(), "OLDEST") == 0)
        return;

    // Cameras whose policy moves a tier out early: source tier -> (camera,
    // newest movable time, destination pool).
    struct MoveRoute {
        std::string camera_id;
        int64_t stable_before;
        std::string dst_pool_id;
    };
    const int64_t now = static_cast<int64_t>(time(nullptr));
    std::unordered_map<std::string, std::vector<MoveRoute>> routes_by_tier;
    for (const auto &item : camera_policies) {
        const auto &policy = item.second;
        if (policy.enabled == 0)
            continue;
        const auto rules = policy.parsedAdvancedRules();
        if (!rules.enable_early_move_when_pool_high)
            continue;
        auto tiers = policy.parsedTiers();
        std::sort(tiers.begin(), tiers.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
            return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
        });
        const int64_t min_age_secs = static_cast<int64_t>(rules.min_segment_age_minutes_before_move) * 60;
        for (size_t i = 0; i < tiers.size(); ++i) {
            const auto &src = tiers[i];
            if (!src.enabled || src.pool_id.empty() ||
                src.overflow_action != overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER)) {
                continue;
            }
            const PolicyTierConfig *dst = nullptr;
            for (size_t j = i + 1; j < tiers.size() && !dst; ++j) {
                if (tiers[j].enabled)
                    dst = &tiers[j];
            }
            if (!dst || dst->pool_id.empty())
                continue;
            const DestinationPoolCheck check = destinationPoolCheck(dst->pool_id);
            if (check.critical || (rules.skip_move_if_pool_offline && !check.has_capacity))
                continue;
            MoveRoute route;
            route.camera_id = item.first;
            route.stable_before = now - std::max<int64_t>(min_age_secs, 60);
            route.dst_pool_id = dst->pool_id;
            routes_by_tier[src.tier].push_back(route);
        }
    }
    if (routes_by_tier.empty())
        return;

    std::vector<StoragePool> pools;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        for (const auto &kv : _pool_cache) {
            if (kv.second.enabled && routes_by_tier.count(kv.second.tier))
                pools.push_back(kv.second);
        }
    }
    SegmentTierRangeImp range_imp;
    TieringJobImp job_imp;
    for (auto pool : pools) {
        if (_stopping)
            return;
        fillPoolRuntimeStats(pool);
        if (pool.total_bytes <= 0 ||
            pool.usage_pct < static_cast<float>(pool.high_watermark_percent) ||
            !isPoolAvailableForTierOperation(pool, false)) {
            continue;
        }
        // Moves already queued from this pool count towards the target.
        const float target_pct = std::max(0.0f, static_cast<float>(pool.high_watermark_percent) - 5.0f);
        const int64_t target_used = static_cast<int64_t>(
            static_cast<double>(pool.total_bytes) * target_pct / 100.0);
        const int64_t need = pool.used_bytes - target_used - job_imp.sumActiveBytesBySourcePool(pool.id);
        if (need <= 0)
            continue;

        std::vector<PressureCameraWindow> windows;
        for (const auto &route : routes_by_tier[pool.tier]) {
            if (route.dst_pool_id == pool.id)
                continue;
            PressureCameraWindow window;
            window.camera_id = route.camera_id;
            window.hi = route.stable_before;
            if (!range_imp.findOldestStartByCameraPool(route.camera_id, pool.id, pool.tier, window.lo) ||
                window.hi <= window.lo) {
                continue;
            }
            windows.push_back(window);
        }
        if (windows.empty())
            continue;

        int64_t estimated = 0;
        const int keep_percent = findBalancedKeepPercent(pool, windows, now, need, true, estimated);
        auto &cutoffs = _pressure_move_cutoffs[pool.id];
        for (const auto &window : windows) {
            const int64_t cutoff = balancedCutoff(window, keep_percent);
            if (cutoff > window.lo)
                cutoffs[window.camera_id] = cutoff;
        }
        DebugL << "Balanced pressure moves from pool " << pool.id << ": keep " << keep_percent
               << "% of " << windows.size() << " camera window(s), need " << need
               << " bytes, estimated " << estimated << " bytes";
    }
}

void TierStorageManager::processCameraPressureTiering(const std::string &camera_id,
                                                      const StoragePolicy &policy) {
    if (policy.enabled == 0)
        return;

    auto tiers = policy.parsedTiers();
    auto rules = policy.parsedAdvancedRules();
    if (!rules.enable_early_move_when_pool_high)
        return;

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t min_age_secs = static_cast<int64_t>(rules.min_segment_age_minutes_before_move) * 60;
    int64_t stable_threshold = now - std::max<int64_t>(min_age_secs, 60);

    std::sort(tiers.begin(), tiers.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
        return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
    });

    SegmentTierRangeImp range_imp;
    GET_CONFIG(int, max_ranges_per_cycle, Storage::kTieringMaxRangesPerCameraCycle);
    if (max_ranges_per_cycle <= 0)
        max_ranges_per_cycle = 64;
    GET_CONFIG(std::string, delete_mode, Storage::kTieringPressureDeleteMode);
    const bool balanced = strcasecmp(delete_mode.c_str(), "OLDEST") != 0;
    auto next_enabled_tier = [&](size_t index) -> const PolicyTierConfig * {
        for (size_t j = index + 1; j < tiers.size(); ++j) {
            if (tiers[j].enabled)
                return &tiers[j];
        }
        return nullptr;
    };

    for (size_t i = 0; i < tiers.size(); ++i) {
        const auto &src_tier_cfg = tiers[i];
        const auto *dst_tier_cfg = next_enabled_tier(i);
        if (!src_tier_cfg.enabled) continue;
        if (src_tier_cfg.overflow_action != overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER))
            continue;
        if (src_tier_cfg.pool_id.empty() || !dst_tier_cfg || dst_tier_cfg->pool_id.empty()) {
            WarnL << "Skip pressure tiering config with empty pool_id camera=" << camera_id
                  << " source_tier=" << src_tier_cfg.tier
                  << " target_tier=" << (dst_tier_cfg ? dst_tier_cfg->tier : "");
            continue;
        }

        if (balanced) {
            // planBalancedPressureMoves() already applied the destination and
            // source watermark checks; move each planned pool's share up to
            // this camera's cutoff.
            for (const auto &planned : _pressure_move_cutoffs) {
                auto cutoff_it = planned.second.find(camera_id);
                if (cutoff_it == planned.second.end() || planned.first == dst_tier_cfg->pool_id)
                    continue;
                auto ranges = range_imp.findPoolRetentionCandidatesAfter(
                    camera_id, planned.first, src_tier_cfg.tier, cutoff_it->second,
                    std::numeric_limits<int64_t>::min(), "", now, max_ranges_per_cycle, true);
                for (const auto &range : ranges) {
                    if (_stopping)
                        return;
                    SegmentTierRange piece;
                    if (!splitRangeForTieringThreshold(range, cutoff_it->second, piece))
                        continue;
                    queueTierMoveJob(piece, src_tier_cfg, *dst_tier_cfg, policy,
                                     TieringJobTrigger::PRESSURE);
                }
            }
            continue;
        }

        StoragePool dst_pool;
        {
            std::lock_guard<std::mutex> lk(_pool_cache_mtx);
            auto dit = _pool_cache.find(dst_tier_cfg->pool_id);
            if (dit == _pool_cache.end())
                continue;
            dst_pool = dit->second;
        }
        if (!dst_pool.enabled)
            continue;

        fillPoolRuntimeStats(dst_pool);
        if (rules.skip_move_if_pool_offline && dst_pool.total_bytes <= 0)
            continue;
        if (dst_pool.usage_pct >= static_cast<float>(dst_pool.critical_watermark_percent))
            continue;

        auto ranges = range_imp.findByCameraTierAndAge(
            camera_id, src_tier_cfg.tier, stable_threshold, max_ranges_per_cycle);
        std::unordered_map<std::string, StoragePool> source_pool_cache;
        std::unordered_map<std::string, int64_t> moved_by_pool;
        std::unordered_map<std::string, bool> reclaimed_pool;
        for (const auto &range : ranges) {
            if (range.status != segmentStatusToString(SegmentStatus::AVAILABLE)) continue;
            if (range.pool_id.empty()) continue;
            if (reclaimed_pool[range.pool_id]) continue;

            StoragePool src_pool;
            auto cached = source_pool_cache.find(range.pool_id);
            if (cached != source_pool_cache.end()) {
                src_pool = cached->second;
            } else {
                std::lock_guard<std::mutex> lk(_pool_cache_mtx);
                auto sit = _pool_cache.find(range.pool_id);
                if (sit == _pool_cache.end())
                    continue;
                src_pool = sit->second;
                if (!src_pool.enabled)
                    continue;
                fillPoolRuntimeStats(src_pool);
                source_pool_cache[src_pool.id] = src_pool;
            }

            if (rules.skip_move_if_pool_offline && src_pool.total_bytes <= 0)
                continue;
            if (src_pool.usage_pct < static_cast<float>(src_pool.high_watermark_percent))
                continue;

            if (queueTierMoveJob(range, src_tier_cfg, *dst_tier_cfg, policy,
                                 TieringJobTrigger::PRESSURE)) {
                moved_by_pool[src_pool.id] += std::max<int64_t>(0, range.size_bytes);
            }

            // Hysteresis: reclaim roughly enough to go below high watermark by 5%.
            auto moved_bytes = moved_by_pool[src_pool.id];
            if (src_pool.total_bytes > 0 && moved_bytes > 0) {
                float target_pct = std::max(0.0f, static_cast<float>(src_pool.high_watermark_percent) - 5.0f);
                int64_t target_used = static_cast<int64_t>(src_pool.total_bytes * target_pct / 100.0f);
                if (src_pool.used_bytes - moved_bytes <= target_used)
                    reclaimed_pool[src_pool.id] = true;
            }
        }
    }
}

void TierStorageManager::enforcePolicyDeleteRetention(const std::string &camera_id,
                                                      const StoragePolicy &policy) {
    if (policy.enabled == 0)
        return;

    auto delete_policy = policy.parsedDeletePolicy();
    if (delete_policy.delete_after_days <= 0)
        return;

    int64_t cutoff = alignTierThreshold(static_cast<int64_t>(time(nullptr)) -
        static_cast<int64_t>(delete_policy.delete_after_days) * 86400);

    SegmentTierRangeImp range_imp;
    const bool move_external =
        delete_policy.delete_mode == deleteModeToString(DeleteMode::MOVE_TO_EXTERNAL_STORAGE);
    // A quality move to the external pool takes protected ranges too: queueTierMoveJob() splits
    // them off and copies them unchanged. Any other mode leaves protected ranges in place.
    PolicyTierConfig external_quality_tier;
    const bool move_protected = move_external &&
        findQualityReductionTier(policy, delete_policy.external_pool_id, external_quality_tier);
    auto ranges = collectAgeRetentionPieces(
        camera_id, cutoff, move_external ? delete_policy.external_pool_id : "",
        !move_protected);
    for (const auto &range : ranges) {
        if (!move_protected && isRangeDeletionProtected(range))
            continue;

        if (delete_policy.delete_mode == deleteModeToString(DeleteMode::MARK_EXPIRED_WAIT_APPROVAL) ||
            delete_policy.require_approval_before_delete) {
            range_imp.updateStatusBySnapshotAt(range, segmentStatusToString(SegmentStatus::EXPIRED),
                                               static_cast<int64_t>(time(nullptr)));
            continue;
        }

        if (delete_policy.delete_mode == deleteModeToString(DeleteMode::MOVE_TO_EXTERNAL_STORAGE)) {
            if (delete_policy.external_pool_id.empty()) {
                WarnL << "Delete policy external_pool_id is empty camera=" << camera_id
                      << " range=" << range.range_id;
                continue;
            }

            StoragePool target_pool;
            bool found_target = false;
            {
                std::lock_guard<std::mutex> lk(_pool_cache_mtx);
                auto it = _pool_cache.find(delete_policy.external_pool_id);
                if (it != _pool_cache.end()) {
                    target_pool = it->second;
                    found_target = true;
                }
            }
            if (!found_target) {
                WarnL << "Delete policy external pool not found: " << delete_policy.external_pool_id;
                continue;
            }
            if (target_pool.id == range.pool_id) {
                WarnL << "Delete policy external pool equals current pool, skip move camera=" << camera_id
                      << " pool_id=" << target_pool.id
                      << " range=" << range.range_id;
                continue;
            }

            PolicyTierConfig src_cfg;
            src_cfg.tier = range.tier;
            src_cfg.enabled = true;
            src_cfg.pool_id = range.pool_id;
            src_cfg.overflow_action = overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER);

            PolicyTierConfig dst_cfg;
            dst_cfg.tier = target_pool.tier;
            dst_cfg.enabled = true;
            dst_cfg.pool_id = target_pool.id;
            if (move_protected)
                dst_cfg = external_quality_tier;
            queueTierMoveJob(range, src_cfg, dst_cfg, policy,
                             TieringJobTrigger::DELETE_POLICY_EXTERNAL);
            continue;
        }

        expireRangeBestEffort(range, segmentStatusToString(SegmentStatus::DELETED));
    }
}

bool TierStorageManager::expireRangeBestEffort(const SegmentTierRange &range,
                                               const std::string &final_status) {
    if (isRangeDeletionProtected(range)) {
        InfoL << "Skip protected range deletion range=" << range.range_id
              << " camera=" << range.camera_id;
        return false;
    }
    if (range.pool_id.empty()) {
        WarnL << "expireRangeBestEffort: range has empty pool_id, range=" << range.range_id
              << " camera=" << range.camera_id;
        return false;
    }
    // A transfer job owns the range's files until it finishes; deleting now
    // would race its copy and final range update.
    if (TieringJobImp().hasActiveJobForRange(range.range_id)) {
        DebugL << "Defer range deletion because a tiering job is active, range="
               << range.range_id;
        return false;
    }
    {
        auto current = SegmentTierRangeImp().findByRangeIds({range.range_id});
        if (current.empty() || !sameRangeSnapshot(current.front(), range)) {
            DebugL << "Skip range deletion because the range changed since it was selected, range="
                   << range.range_id;
            return false;
        }
    }

    StoragePool pool;
    bool has_pool = false;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto it = _pool_cache.find(range.pool_id);
        if (it != _pool_cache.end()) {
            pool = it->second;
            has_pool = true;
        }
    }
    if (!has_pool) {
        WarnL << "expireRangeBestEffort: pool not found, pool_id=" << range.pool_id
              << " range=" << range.range_id;
        return false;
    }
    PoolHealthInfo health;
    if (!isPoolAvailableForTierOperation(pool, false, &health)) {
        DebugL << "Defer range deletion because storage pool is unavailable: pool="
               << pool.id << " status=" << health.health_status
               << " stale=" << health.stale << " range=" << range.range_id;
        return false;
    }

    std::vector<TierRangeSegmentFile> segments;
    std::string collect_error;
    if (!collectSegmentFilesForRange(range, segments, collect_error)) {
        WarnL << "Defer range deletion because its segments could not be enumerated, range="
              << range.range_id << ": " << collect_error;
        return false;
    }
    if (segments.empty()) {
        WarnL << "Refuse to finalize deletion because no exact segments were enumerated, range="
              << range.range_id;
        return false;
    }
    bool all_deleted = true;

    if (has_pool && poolTypeIsObjectStorage(pool.type)) {
        std::string base_path = pool.base_path.value_or("");
        if (!_obj_storage.isRegistered(pool.id))
            registerPoolObjStorage(pool);
        std::vector<std::string> keys;
        keys.reserve(segments.size());
        for (const auto &seg : segments) {
            if (seg.stream_id != range.stream_id) continue;
            auto key = TierObjectStorage::makeS3Key(base_path, seg.camera_id, seg.stream_id,
                                                    seg.segment_path);
            // A segment uploaded while encrypted sits under the ".enc" spelling; ask for
            // both so a key written by the other variant is not left behind.
            if (s_encryption_active.load(std::memory_order_relaxed))
                keys.push_back(key + RecordEncryptionUtils::encryptedFileSuffix());
            keys.push_back(std::move(key));
        }
        // DeleteObjects is idempotent for absent keys; any request or per-key
        // error keeps the range AVAILABLE so the next cycle retries every key.
        if (!_obj_storage.deleteSegmentsBatch(pool.id, keys))
            all_deleted = false;
    } else if (has_pool) {
        auto storage = fileStorageForPoolType(pool.type);
        if (!storage) {
            WarnL << "expireRangeBestEffort: unsupported file storage type " << pool.type
                  << " pool=" << pool.id;
            return false;
        }
        if (!storage->isRegistered(pool.id))
            registerPoolFileStorage(pool);
        for (const auto &seg : segments) {
            if (seg.stream_id != range.stream_id) continue;
            // File backend treats ENOENT as success, making retries idempotent.
            if (s_encryption_active.load(std::memory_order_relaxed)) {
                // The counterpart spelling may still be on disk if the file was
                // encrypted or decrypted since the key was built; drop it too.
                const std::string other = RecordEncryptionUtils::toggleEncSuffix(seg.storage_key);
                if (other != seg.storage_key)
                    storage->deleteSegment(pool.id, other);
            }
            if (!storage->deleteSegment(pool.id, seg.storage_key))
                all_deleted = false;
        }
        pruneEmptySegmentDateDirectories(configuredPoolDataRoot(pool), segments);
    }

    if (!all_deleted) {
        WarnL << "Failed to delete every segment for range " << range.range_id
              << " pool=" << range.pool_id;
        // The range stays AVAILABLE and is retried every cycle: alert once per pool per hour.
        const int64_t alert_now = static_cast<int64_t>(time(nullptr));
        bool should_alert = false;
        {
            std::lock_guard<std::mutex> lk(_delete_failed_alert_mtx);
            auto &alerted_at = _delete_failed_alerted_at[range.pool_id];
            if (alert_now - alerted_at >= 3600) {
                alerted_at = alert_now;
                should_alert = true;
            }
        }
        if (should_alert) {
            StorageAlert alert;
            alert.id = StrUUID::make_guid(8, "alert");
            alert.level = "ERROR";
            alert.type = "TIER_DELETE_FAILED";
            alert.pool_id = range.pool_id;
            alert.message = "Failed to delete every segment for range " + range.range_id;
            alert.created_at = alert_now;
            StorageAlertImp().add(alert);
        }
        return false;
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    TierMetadataCleanupJob cleanup_job;
    cleanup_job.range_id = range.range_id;
    cleanup_job.camera_id = range.camera_id;
    cleanup_job.status = "PENDING";
    cleanup_job.created_at = now;
    cleanup_job.updated_at = now;
    if (!SegmentTierRangeImp().markDeletedBySnapshotAndEnqueue(
            range,
            final_status.empty() ? segmentStatusToString(SegmentStatus::DELETED) : final_status,
            cleanup_job)) {
        WarnL << "Deleted range files but the range changed before it could be finalized, range="
              << range.range_id;
        return false;
    }
    return true;
}

bool TierStorageManager::isRangeDeletionProtected(const SegmentTierRange &range) const {
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (SegmentRetentionOverrideImp().isActive(range.range_id, now))
        return true;
    return ProtectedVideoImp().overlaps(range.camera_id, range.start_time, range.end_time);
}

void TierStorageManager::processMetadataCleanupJobs() {
    TierMetadataCleanupJobImp cleanup_imp;
    SegmentTierRangeImp range_imp;
    // Each job needs two passes (remove, then verify). The limit must stay well
    // above the ranges finished per cycle or the backlog grows without bound.
    auto jobs = cleanup_imp.queryPending(2000);

    // One timefile rewrite per camera per cycle, however many ranges it has.
    std::map<std::string, std::vector<TimeBlockIdentity>> identities_by_camera;
    std::map<std::string, std::vector<std::string>> jobs_by_camera;
    for (const auto &job : jobs) {
        if (_stopping)
            return;
        try {
            auto ranges = range_imp.findByRangeIds({job.range_id});
            if (ranges.empty()) {
                cleanup_imp.updateResult(job.range_id, false, "Metadata missing for pending cleanup job");
                continue;
            }
            const auto &range = ranges.front();
            if (job.camera_id != range.camera_id) {
                cleanup_imp.updateResult(job.range_id, false, "Camera ID mismatch for pending cleanup job");
                continue;
            }
            std::vector<TierRangeSegmentFile> segments;
            std::string collect_error;
            if (!collectSegmentFilesForRange(range, segments, collect_error)) {
                cleanup_imp.updateResult(job.range_id, false,
                                         "Cannot read timefile: " + collect_error);
                continue;
            }
            // TimeRebuilder swaps files asynchronously after active readers
            // drain. Do not run later metadata steps until a subsequent cycle
            // verifies that every TimeBlock identity for this range is gone.
            if (segments.empty()) {
                bool completed = cleanupRangeMotionMetadata(range) &&
                                 cleanupRangeBookmarkMetadata(range);
                cleanup_imp.updateResult(job.range_id, completed, completed ? "" : "Invalid metadata cleanup range");
                continue;
            }
            auto &identities = identities_by_camera[range.camera_id];
            for (const auto &segment : segments) {
                TimeBlockIdentity identity;
                identity.camera_id = segment.camera_id;
                identity.stream_id = segment.stream_id;
                identity.start_time = static_cast<uint64_t>(segment.start_time);
                identity.segment_path = segment.segment_path;
                identities.push_back(identity);
            }
            jobs_by_camera[range.camera_id].push_back(job.range_id);
        } catch (const std::exception &ex) {
            cleanup_imp.updateResult(job.range_id, false, ex.what());
        } catch (...) {
            cleanup_imp.updateResult(job.range_id, false, "Unknown metadata cleanup error");
        }
    }

    for (const auto &kv : identities_by_camera) {
        std::string error = "Awaiting timefile swap verification";
        try {
            StorageManager::Instance().removeCameraTimeBlocks(kv.first, kv.second);
        } catch (const std::exception &ex) {
            error = ex.what();
        } catch (...) {
            error = "Unknown timefile cleanup error";
        }
        for (const auto &range_id : jobs_by_camera[kv.first])
            cleanup_imp.updateResult(range_id, false, error);
    }
}

bool TierStorageManager::cleanupRangeMotionMetadata(const SegmentTierRange &range) {
#if defined(ENABLE_MOTION)
    auto range_days = localDaysForWindow(range.start_time, range.end_time);
    if (range_days.empty())
        return false;

    MediaTuple tuple;
    tuple.app = range.camera_id;
    const std::string timeline_dir = cameraTimelineDir(range.camera_id);
    std::vector<uint64_t> empty_days;
    for (auto day_start : range_days) {
        auto day_end = nextLocalDayStart(day_start);
        const std::string files = timelineFilesForWindow(
            timeline_dir, static_cast<int64_t>(day_start), static_cast<int64_t>(day_end) - 1);
        if (files.empty()) {
            empty_days.push_back(day_start);
            continue;
        }
        TimeQuery query(tuple, files);
        query.getRecordedTimePeriod(day_start, day_end, [&](const std::vector<TimeBlock> &blocks) {
            if (blocks.empty())
                empty_days.push_back(day_start);
        });
    }
    if (!empty_days.empty())
        MotionBlockManager::Instance().removeMotionBlockDays(range.camera_id, empty_days);
#else
    (void)range;
#endif
    return true;
}

bool TierStorageManager::cleanupRangeBookmarkMetadata(const SegmentTierRange &range) {
    if (range.camera_id.empty() || range.start_time <= 0 || range.end_time < range.start_time)
        return false;
    BookmarkImp().removeByTimeRange(
        static_cast<uint64_t>(range.start_time),
        static_cast<uint64_t>(range.end_time),
        range.camera_id);
    return true;
}

void TierStorageManager::enforceCameraArchiveRetention(const std::string &camera_id,
                                                       const StoragePolicy &) {
    auto recorder = StatisticRecorder::Instance().getRecorder(camera_id, false);
    if (!recorder)
        return;

    auto params = recorder->getParams();
    int64_t now = static_cast<int64_t>(time(nullptr));

    // keepArchivedMaxFor keeps its original meaning: when explicit, data older
    // than this maximum retention is eligible for expiry even if storage is
    // otherwise available.
    if (!params.option.keepArchivedMaxForAuto && params.option.keepArchivedMaxFor > 0) {
        int64_t max_cutoff = alignTierThreshold(now - static_cast<int64_t>(params.option.keepArchivedMaxFor));
        for (const auto &range : collectAgeRetentionPieces(camera_id, max_cutoff, ""))
            expireRangeBestEffort(range);
    }
}

std::vector<SegmentTierRange> TierStorageManager::collectAgeRetentionPieces(
        const std::string &camera_id,
        int64_t cutoff,
        const std::string &exclude_pool_id,
        bool carve_protected) {
    std::vector<SegmentTierRange> pieces;
    GET_CONFIG(int, max_ranges_per_cycle, Storage::kTieringMaxRangesPerCameraCycle);
    if (max_ranges_per_cycle <= 0)
        max_ranges_per_cycle = 64;
    const int max_pages = 16;
    const int64_t now = static_cast<int64_t>(time(nullptr));

    SegmentTierRangeImp range_imp;
    int64_t after_start = std::numeric_limits<int64_t>::min();
    std::string after_id;
    for (int page = 0; page < max_pages && !_stopping; ++page) {
        auto ranges = range_imp.findRetentionCandidatesAfter(
            camera_id, cutoff, after_start, after_id, now, exclude_pool_id,
            max_ranges_per_cycle);
        if (ranges.empty())
            break;
        for (const auto &range : ranges) {
            after_start = range.start_time;
            after_id = range.range_id;
            // Ranges are day-sized; cut at the cutoff so the old part expires
            // on time. The newer remainder is picked up by later cycles.
            SegmentTierRange piece;
            if (!splitRangeForTieringThreshold(range, cutoff, piece))
                continue;
            if (!carve_protected) {
                pieces.push_back(piece);
                if (static_cast<int>(pieces.size()) >= max_ranges_per_cycle)
                    return pieces;
                continue;
            }
            std::vector<SegmentTierRange> deletable;
            if (!carveDeletablePieces(piece, deletable))
                continue;
            for (const auto &item : deletable)
                pieces.push_back(item);
            if (static_cast<int>(pieces.size()) >= max_ranges_per_cycle)
                return pieces;
        }
    }
    return pieces;
}

bool TierStorageManager::carveDeletablePieces(const SegmentTierRange &range,
                                              std::vector<SegmentTierRange> &out) {
    out.clear();
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (SegmentRetentionOverrideImp().isActive(range.range_id, now))
        return true;
    auto protections = ProtectedVideoImp().findOverlapping(
        range.camera_id, range.start_time, range.end_time);
    if (protections.empty()) {
        out.push_back(range);
        return true;
    }

    // Skip the timefile read when protections cover the whole window.
    {
        int64_t covered_until = range.start_time;
        for (const auto &video : protections) {
            if (video.start_time > covered_until)
                break;
            covered_until = std::max(covered_until, video.end_time);
        }
        if (covered_until >= range.end_time)
            return true;
    }

    std::vector<TierRangeSegmentFile> segments;
    std::string error;
    if (!collectSegmentFilesForRange(range, segments, error) || segments.empty())
        return false;

    // Protections use inclusive bounds (ProtectedVideoRepository::overlaps).
    auto touchesProtection = [&](int64_t start, int64_t end) {
        for (const auto &video : protections) {
            if (video.start_time <= end && video.end_time >= start)
                return true;
        }
        return false;
    };

    const int kDeletable = 1;
    const int kProtected = 2;
    std::vector<int> kinds(segments.size(), kDeletable);
    bool any_deletable = false;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (touchesProtection(segments[i].start_time, segments[i].end_time))
            kinds[i] = kProtected;
    }
    std::vector<ShapedPiece> shaped;
    // A protection sitting in a gap would make a widened window touch it.
    if (!shapeRangePieces(range, segments, kinds, touchesProtection, now, shaped))
        return false;
    for (const auto &piece : shaped)
        any_deletable = any_deletable || piece.kind == kDeletable;
    if (!any_deletable)
        return true;

    // A single piece whose window did not shrink needs no rewrite.
    if (shaped.size() == 1 && shaped.front().range.start_time == range.start_time &&
        shaped.front().range.end_time == range.end_time) {
        out.push_back(range);
        return true;
    }
    std::vector<SegmentTierRange> pieces;
    for (const auto &piece : shaped)
        pieces.push_back(piece.range);
    if (!SegmentTierRangeImp().replaceRangeWithPiecesIfUnchanged(range, pieces)) {
        WarnL << "Cannot split range around protected videos, range=" << range.range_id;
        return false;
    }
    for (const auto &piece : shaped) {
        if (piece.kind == kDeletable)
            out.push_back(piece.range);
    }
    DebugL << "Split range " << range.range_id << " around protected videos into "
           << pieces.size() << " piece(s), deletable=" << out.size();
    return true;
}

bool TierStorageManager::isFilePoolVerifiablyReadable(const StoragePool &pool) const {
    if (!pool.enabled || poolTypeIsObjectStorage(pool.type))
        return false;
    if (!isPoolAvailableForTierOperation(pool, false))
        return false;
    // An unmounted NAS still shows an empty mount directory; the configured
    // data root must exist before absent files can be trusted as lost.
    const std::string root = configuredPoolDataRoot(pool);
    struct stat root_st{};
    return !root.empty() && ::stat(root.c_str(), &root_st) == 0 && S_ISDIR(root_st.st_mode);
}

bool TierStorageManager::reconcileRangeFiles(const SegmentTierRange &range, int64_t verify_before) {
    StoragePool pool;
    if (!getPoolById(range.pool_id, pool) || !isFilePoolVerifiablyReadable(pool))
        return false;

    std::vector<TierRangeSegmentFile> segments;
    std::string error;
    if (!collectSegmentFilesForRange(range, segments, error))
        return false;
    if (segments.empty())
        return true;

    const int kPresent = 1;
    const int kMissing = 0;
    std::vector<int> kinds(segments.size(), kPresent);
    std::vector<TierRangeSegmentFile> missing;
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto &segment = segments[i];
        if (segment.end_time > verify_before)
            continue;
        struct stat st{};
        if (::stat(segment.full_path.c_str(), &st) == 0)
            continue;
        if (errno != ENOENT) {
            WarnL << "Skip range reconciliation, cannot stat " << segment.full_path
                  << ": " << strerror(errno);
            return false;
        }
        kinds[i] = kMissing;
        missing.push_back(segment);
    }
    if (missing.empty())
        return true;
    return dropMissingSegments(range, segments, kinds, missing, "source and target");
}

bool TierStorageManager::dropMissingSegments(const SegmentTierRange &range,
                                             const std::vector<TierRangeSegmentFile> &segments,
                                             const std::vector<int> &kinds,
                                             const std::vector<TierRangeSegmentFile> &missing,
                                             const std::string &missing_from) {
    const int64_t now = static_cast<int64_t>(time(nullptr));
    SegmentTierRangeImp range_imp;
    std::vector<ShapedPiece> shaped;
    if (!shapeRangePieces(range, segments, kinds, nullptr, now, shaped))
        return false;
    bool updated = false;
    if (shaped.empty()) {
        updated = range_imp.updateStatusBySnapshotAt(
            range, segmentStatusToString(SegmentStatus::MISSING), now);
    } else {
        std::vector<SegmentTierRange> pieces;
        for (const auto &piece : shaped)
            pieces.push_back(piece.range);
        updated = range_imp.replaceRangeWithPiecesIfUnchanged(range, pieces);
    }
    if (!updated) {
        DebugL << "Range changed during reconciliation, retry later: " << range.range_id;
        return false;
    }
    InfoL << "Reconciled range " << range.range_id << " camera=" << range.camera_id
          << ": dropped " << missing.size() << " segment(s) missing from " << missing_from
          << ", pieces=" << shaped.size();
    reportMissingSegments(range, missing, missing_from);
    return true;
}

void TierStorageManager::runRangeFileReconcile() {
    GET_CONFIG(bool, enabled, Storage::kTierReconcileEnabled);
    GET_CONFIG(int64_t, work_budget_ms, Storage::kTierReconcileWorkBudgetMs);
    GET_CONFIG(int64_t, min_age_seconds, Storage::kTierReconcileMinAgeSeconds);
    if (!enabled)
        return;
    if (work_budget_ms <= 0)
        work_budget_ms = 2000;
    if (min_age_seconds < 0)
        min_age_seconds = 0;

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int64_t verify_before = now - min_age_seconds;
    std::unordered_map<std::string, bool> pool_readable;
    auto readable = [&](const std::string &pool_id) {
        auto it = pool_readable.find(pool_id);
        if (it != pool_readable.end())
            return it->second;
        StoragePool pool;
        const bool ok = getPoolById(pool_id, pool) && isFilePoolVerifiablyReadable(pool);
        pool_readable[pool_id] = ok;
        return ok;
    };

    // Newest first: recent inconsistencies (e.g. left by an upgrade) matter
    // most, and old ranges are also fixed by retention deleting them.
    SegmentTierRangeImp range_imp;
    TieringJobImp job_imp;
    Ticker budget;
    size_t checked = 0;
    bool wrapped = false;
    while (!_stopping && budget.elapsedTime() < static_cast<uint64_t>(work_budget_ms)) {
        auto ranges = range_imp.findReconcileCandidatesBefore(
            _reconcile_cursor_start, _reconcile_cursor_id, 32);
        if (ranges.empty()) {
            _reconcile_cursor_start = std::numeric_limits<int64_t>::max();
            _reconcile_cursor_id.clear();
            if (wrapped)
                break;
            wrapped = true;
            continue;
        }
        for (const auto &range : ranges) {
            if (_stopping || budget.elapsedTime() >= static_cast<uint64_t>(work_budget_ms))
                break;
            _reconcile_cursor_start = range.start_time;
            _reconcile_cursor_id = range.range_id;
            if (range.start_time > verify_before || !readable(range.pool_id))
                continue;
            if (job_imp.hasActiveJobForRange(range.range_id))
                continue;
            reconcileRangeFiles(range, verify_before);
            ++checked;
        }
    }
    if (checked > 0) {
        DebugL << "Range file reconciliation checked " << checked << " range(s) in "
               << budget.elapsedTime() << " ms";
    }
}

// Unknown values fall back to REPORT: never change data on a typo.
static const char *objectReconcileMode() {
    GET_CONFIG(std::string, mode, Storage::kTierObjectReconcileMode);
    if (strcasecmp(mode.c_str(), "OFF") == 0)
        return "OFF";
    if (strcasecmp(mode.c_str(), "FIX") == 0)
        return "FIX";
    return "REPORT";
}

static void addStorageAlert(const std::string &level,
                            const std::string &type,
                            const std::string &pool_id,
                            const std::string &message) {
    StorageAlert alert;
    alert.id = StrUUID::make_guid(8, "alert");
    alert.level = level;
    alert.type = type;
    alert.pool_id = pool_id;
    alert.message = message;
    alert.created_at = static_cast<int64_t>(time(nullptr));
    StorageAlertImp().add(alert);
}

TierStorageManager::ObjectRangeCheck
TierStorageManager::checkObjectRange(const StoragePool &pool,
                                     const SegmentTierRange &range,
                                     int64_t verify_before,
                                     ObjectRangeFinding &out) {
    out = ObjectRangeFinding();
    out.range = range;
    std::string error;
    if (!collectSegmentFilesForRange(range, out.segments, error))
        return ObjectRangeCheck::SKIPPED;
    if (out.segments.empty())
        return ObjectRangeCheck::CONSISTENT;

    // Ranges split at local midnight, so their keys span one or two day prefixes.
    const std::string base_path = pool.base_path.value_or("");
    std::vector<std::string> keys;
    std::set<std::string> prefixes;
    for (const auto &segment : out.segments) {
        keys.push_back(TierObjectStorage::makeS3Key(base_path, segment.camera_id,
                                                    segment.stream_id, segment.segment_path));
        if (segment.end_time <= verify_before)
            prefixes.insert(keys.back().substr(0, keys.back().rfind('/') + 1));
    }
    std::map<std::string, TierObjectInfo> listed;
    for (const auto &prefix : prefixes) {
        std::map<std::string, TierObjectInfo> objects;
        if (!_obj_storage.listObjects(pool.id, prefix, objects, error))
            return ObjectRangeCheck::ERROR;
        listed.insert(objects.begin(), objects.end());
    }

    out.kinds.assign(out.segments.size(), 1);
    size_t size_mismatches = 0;
    std::string first_mismatch;
    for (size_t i = 0; i < out.segments.size(); ++i) {
        const auto &segment = out.segments[i];
        if (segment.end_time > verify_before)
            continue;
        std::string listed_key;
        const TierObjectInfo *listed_object = findListedObjectVariant(listed, keys[i], listed_key);
        if (listed_object) {
            if (!objectSizeMatchesSegment(listed_object->size, segment.file_size,
                                          RecordEncryptionUtils::hasEncSuffix(listed_key))) {
                if (first_mismatch.empty())
                    first_mismatch = listed_key;
                ++size_mismatches;
            }
            continue;
        }
        // A listing can lag behind writes; only a 404 proves the loss, and it has to be a 404
        // for both spellings before the segment is dropped.
        const TierObjectPresence plain_presence = _obj_storage.headSegment(pool.id, keys[i]);
        if (plain_presence == TierObjectPresence::PRESENT)
            continue;
        const TierObjectPresence enc_presence = _obj_storage.headSegment(
            pool.id, keys[i] + RecordEncryptionUtils::encryptedFileSuffix());
        if (enc_presence == TierObjectPresence::PRESENT)
            continue;
        if (plain_presence != TierObjectPresence::ABSENT ||
            enc_presence != TierObjectPresence::ABSENT)
            return ObjectRangeCheck::ERROR;
        out.kinds[i] = 0;
        out.missing.push_back(segment);
        if (out.first_missing_key.empty())
            out.first_missing_key = keys[i];
    }
    if (size_mismatches > 0 && _object_reconcile_reported.insert("size:" + range.range_id).second) {
        addStorageAlert("WARNING", "TIER_OBJECT_SIZE_MISMATCH", pool.id,
                        StrPrinter << size_mismatches
                                   << " object(s) differ in size from the recorded segment, range="
                                   << range.range_id << ", first=" << first_mismatch);
    }
    if (out.missing.empty())
        return ObjectRangeCheck::CONSISTENT;
    return out.missing.size() == out.segments.size() ? ObjectRangeCheck::ALL_MISSING
                                                     : ObjectRangeCheck::PARTIAL;
}

void TierStorageManager::applyObjectFinding(const StoragePool &pool, const ObjectRangeFinding &finding) {
    const auto &range = finding.range;
    // A job queued since the check owns the range now.
    if (TieringJobImp().hasActiveJobForRange(range.range_id))
        return;
    if (strcmp(objectReconcileMode(), "FIX") != 0) {
        if (_object_reconcile_reported.insert("missing:" + range.range_id).second) {
            addStorageAlert("WARNING", "TIER_OBJECT_MISSING", pool.id,
                            StrPrinter << finding.missing.size() << " of " << finding.segments.size()
                                       << " segment(s) missing from object storage, range="
                                       << range.range_id << ", first=" << finding.first_missing_key
                                       << " (REPORT mode, range unchanged)");
        }
        return;
    }
    if (dropMissingSegments(range, finding.segments, finding.kinds, finding.missing,
                            "object storage")) {
        std::lock_guard<std::mutex> lk(_quota_usage_mtx);
        _quota_usage.erase(pool.id);
    }
}

void TierStorageManager::flushPendingObjectFindings(const StoragePool &pool,
                                                    ObjectReconcilePoolState &state) {
    std::vector<ObjectRangeFinding> pending;
    pending.swap(state.pending_all_missing);
    for (const auto &finding : pending) {
        if (_stopping)
            return;
        applyObjectFinding(pool, finding);
    }
}

void TierStorageManager::tripObjectMassLossGuard(const StoragePool &pool,
                                                 ObjectReconcilePoolState &state) {
    GET_CONFIG(int64_t, pause_seconds, Storage::kTierObjectReconcilePauseSeconds);
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const std::string first = state.pending_all_missing.empty()
                                  ? std::string()
                                  : state.pending_all_missing.front().first_missing_key;
    const size_t count = state.pending_all_missing.size();
    WarnL << "Object reconciliation paused on pool " << pool.id << ": " << count
          << " range(s) in a row have no object at all, first=" << first;
    addStorageAlert("ERROR", "POOL_OBJECTS_MISSING_SUSPECT", pool.id,
                    StrPrinter << count << " range(s) in a row have no object in bucket '"
                               << pool.bucket.value_or("") << "' under base_path '"
                               << pool.base_path.value_or("") << "' (first=" << first
                               << "). Nothing was changed; check the bucket, base_path and "
                                  "credentials. Reconciliation of this pool is paused for "
                               << pause_seconds << " s");
    const std::string fingerprint = state.fingerprint;
    state = ObjectReconcilePoolState();
    state.fingerprint = fingerprint;
    state.paused_until = now + std::max<int64_t>(0, pause_seconds);
}

TierStorageManager::ObjectRangeCheck
TierStorageManager::reconcileObjectRange(const StoragePool &pool,
                                         ObjectReconcilePoolState &state,
                                         const SegmentTierRange &range,
                                         int64_t verify_before) {
    GET_CONFIG(int, mass_loss_threshold, Storage::kTierObjectReconcileMassLossThreshold);
    if (mass_loss_threshold <= 0)
        mass_loss_threshold = 20;
    ObjectRangeFinding finding;
    const ObjectRangeCheck check = checkObjectRange(pool, range, verify_before, finding);
    switch (check) {
    case ObjectRangeCheck::CONSISTENT:
    case ObjectRangeCheck::PARTIAL:
        // A present object shows the bucket and base_path are right, so the
        // fully missing ranges seen before it are real losses.
        state.seen_present = true;
        flushPendingObjectFindings(pool, state);
        if (check == ObjectRangeCheck::PARTIAL)
            applyObjectFinding(pool, finding);
        break;
    case ObjectRangeCheck::ALL_MISSING:
        state.pending_all_missing.push_back(finding);
        if (state.pending_all_missing.size() >= static_cast<size_t>(mass_loss_threshold))
            tripObjectMassLossGuard(pool, state);
        break;
    default:
        break;
    }
    return check;
}

// Local midnight bounds of a "YYYY-MM-DD" day directory; false for any other name.
static bool parseLocalDay(const std::string &day, int64_t &out_start, int64_t &out_end) {
    int year = 0, month = 0, mday = 0;
    char tail = 0;
    if (day.size() != 10 || std::sscanf(day.c_str(), "%4d-%2d-%2d%c", &year, &month, &mday, &tail) != 3)
        return false;
    if (month < 1 || month > 12 || mday < 1 || mday > 31)
        return false;
    struct tm tm_day{};
    tm_day.tm_year = year - 1900;
    tm_day.tm_mon = month - 1;
    tm_day.tm_mday = mday;
    tm_day.tm_isdst = -1;
    struct tm tm_next = tm_day;
    ++tm_next.tm_mday;
    out_start = static_cast<int64_t>(mktime(&tm_day));
    out_end = static_cast<int64_t>(mktime(&tm_next));
    return out_start > 0 && out_end > out_start;
}

bool TierStorageManager::scanOrphanDayPrefix(const StoragePool &pool,
                                             const std::string &prefix,
                                             const std::string &camera_id,
                                             const std::string &stream_id,
                                             const std::string &day,
                                             ObjectReconcilePoolState &state) {
    int64_t day_start = 0;
    int64_t day_end = 0;
    if (!parseLocalDay(day, day_start, day_end))
        return true;
    std::map<std::string, TierObjectInfo> objects;
    std::string error;
    if (!_obj_storage.listObjects(pool.id, prefix, objects, error))
        return false;
    if (objects.empty())
        return true;

    // Keys owned by the pool's ranges around this day (an hour of slack for
    // clock changes); an unreadable timefile makes the whole day owned.
    const std::string base_path = pool.base_path.value_or("");
    std::set<std::string> owned;
    for (const auto &range : SegmentTierRangeImp().findStoredByCameraStreamPool(
             camera_id, stream_id, pool.id, day_start - 3600, day_end + 3600)) {
        std::vector<TierRangeSegmentFile> segments;
        if (!collectSegmentFilesForRange(range, segments, error))
            return true;
        for (const auto &segment : segments) {
            const std::string key = TierObjectStorage::makeS3Key(base_path, segment.camera_id,
                                                                 segment.stream_id, segment.segment_path);
            owned.insert(key);
            // The same segment sits under the ".enc" spelling once it was uploaded encrypted;
            // neither spelling is an orphan.
            owned.insert(key + RecordEncryptionUtils::encryptedFileSuffix());
        }
    }

    // Uploads write the object before the range moves to this pool.
    GET_CONFIG(int64_t, min_age_seconds, Storage::kTierObjectOrphanMinAgeSeconds);
    const int64_t now = static_cast<int64_t>(time(nullptr));
    for (const auto &object : objects) {
        if (owned.count(object.first) || now - object.second.last_modified < min_age_seconds)
            continue;
        if (state.orphan_first.empty())
            state.orphan_first = object.first;
        ++state.orphan_objects;
        state.orphan_bytes += std::max<int64_t>(0, object.second.size);
    }
    return true;
}

void TierStorageManager::runObjectOrphanSweep(const StoragePool &pool,
                                              ObjectReconcilePoolState &state,
                                              const std::function<bool()> &exhausted) {
    const int64_t now = static_cast<int64_t>(time(nullptr));
    std::string root = pool.base_path.value_or("");
    if (!root.empty() && root.back() != '/')
        root += '/';
    if (!state.orphan_active) {
        if (now < state.orphan_next_sweep)
            return;
        state.orphan_active = true;
        state.orphan_queue.clear();
        state.orphan_queue.push_back(std::make_pair(root, 0));
        state.orphan_objects = 0;
        state.orphan_bytes = 0;
        state.orphan_first.clear();
    }

    while (!state.orphan_queue.empty() && !exhausted()) {
        const std::pair<std::string, int> item = state.orphan_queue.front();
        if (item.second < 3) {
            std::vector<std::string> children;
            std::string error;
            if (!_obj_storage.listCommonPrefixes(pool.id, item.first, children, error))
                return;
            state.orphan_queue.pop_front();
            for (const auto &child : children)
                state.orphan_queue.push_back(std::make_pair(child, item.second + 1));
            continue;
        }
        // "<root>camera/stream/YYYY-MM-DD/"
        std::vector<std::string> parts;
        std::string rest = item.first.substr(root.size());
        size_t pos = 0;
        while (pos < rest.size()) {
            const size_t slash = rest.find('/', pos);
            if (slash == std::string::npos)
                break;
            parts.push_back(rest.substr(pos, slash - pos));
            pos = slash + 1;
        }
        if (parts.size() == 3 &&
            !scanOrphanDayPrefix(pool, item.first, parts[0], parts[1], parts[2], state)) {
            return;
        }
        state.orphan_queue.pop_front();
    }
    if (!state.orphan_queue.empty())
        return;

    GET_CONFIG(int64_t, min_age_seconds, Storage::kTierObjectOrphanMinAgeSeconds);
    GET_CONFIG(int64_t, sweep_interval_seconds, Storage::kTierObjectOrphanSweepIntervalSeconds);
    if (state.orphan_objects > 0) {
        WarnL << "Pool " << pool.id << " holds " << state.orphan_objects << " orphan object(s), "
              << state.orphan_bytes << " bytes, first=" << state.orphan_first;
        addStorageAlert("WARNING", "POOL_ORPHAN_OBJECTS", pool.id,
                        StrPrinter << state.orphan_objects << " object(s), " << state.orphan_bytes
                                   << " bytes under '" << root << "' in bucket '"
                                   << pool.bucket.value_or("")
                                   << "' belong to no range of this pool and are older than "
                                   << min_age_seconds << " s (first=" << state.orphan_first
                                   << "). They were not deleted");
    }
    state.orphan_active = false;
    state.orphan_next_sweep = now + std::max<int64_t>(0, sweep_interval_seconds);
}

void TierStorageManager::requestObjectRangeReconcile(const std::string &range_id) {
    if (range_id.empty())
        return;
    std::lock_guard<std::mutex> lk(_object_reconcile_priority_mtx);
    _object_reconcile_priority.insert(range_id);
}

void TierStorageManager::runObjectRangeReconcile() {
    if (strcmp(objectReconcileMode(), "OFF") == 0)
        return;
    GET_CONFIG(int64_t, work_budget_ms, Storage::kTierObjectReconcileWorkBudgetMs);
    GET_CONFIG(int64_t, min_age_seconds, Storage::kTierReconcileMinAgeSeconds);
    if (work_budget_ms <= 0)
        work_budget_ms = 2000;
    if (min_age_seconds < 0)
        min_age_seconds = 0;

    std::vector<StoragePool> pools;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        for (const auto &kv : _pool_cache) {
            if (kv.second.enabled && poolTypeIsObjectStorage(kv.second.type))
                pools.push_back(kv.second);
        }
    }
    std::sort(pools.begin(), pools.end(),
              [](const StoragePool &a, const StoragePool &b) { return a.id < b.id; });
    for (auto it = _object_reconcile.begin(); it != _object_reconcile.end();) {
        const bool known = std::any_of(pools.begin(), pools.end(),
                                       [&](const StoragePool &pool) { return pool.id == it->first; });
        it = known ? std::next(it) : _object_reconcile.erase(it);
    }
    if (_object_reconcile_reported.size() > 100000)
        _object_reconcile_reported.clear();
    if (pools.empty())
        return;

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int64_t verify_before = now - min_age_seconds;
    // A changed endpoint/bucket/base_path invalidates everything seen so far,
    // in particular fully missing ranges found under the old location.
    auto usable = [&](const StoragePool &pool, ObjectReconcilePoolState &state) {
        const std::string fingerprint = pool.endpoint.value_or("") + "|" + pool.bucket.value_or("") +
                                        "|" + pool.base_path.value_or("");
        if (state.fingerprint != fingerprint) {
            state = ObjectReconcilePoolState();
            state.fingerprint = fingerprint;
        }
        if (now < state.paused_until)
            return false;
        if (!_obj_storage.isRegistered(pool.id) && !registerPoolObjStorage(pool))
            return false;
        return isPoolAvailableForTierOperation(pool, false);
    };

    SegmentTierRangeImp range_imp;
    TieringJobImp job_imp;
    Ticker budget;
    auto exhausted = [&]() {
        return _stopping || budget.elapsedTime() >= static_cast<uint64_t>(work_budget_ms);
    };
    size_t checked = 0;

    // Ranges queued by a failed restore go first (at most 32 per cycle); one
    // that cannot be checked now (listing error) stays queued, anything else
    // the sweep covers later.
    std::vector<std::string> priority;
    {
        std::lock_guard<std::mutex> lk(_object_reconcile_priority_mtx);
        while (!_object_reconcile_priority.empty() && priority.size() < 32) {
            priority.push_back(*_object_reconcile_priority.begin());
            _object_reconcile_priority.erase(_object_reconcile_priority.begin());
        }
    }
    std::vector<std::string> retry;
    for (const auto &range : priority.empty() ? std::vector<SegmentTierRange>()
                                              : range_imp.findByRangeIds(priority)) {
        if (_stopping)
            break;
        auto pool_it = std::find_if(pools.begin(), pools.end(), [&](const StoragePool &pool) {
            return pool.id == range.pool_id;
        });
        if (pool_it == pools.end() || range.status != segmentStatusToString(SegmentStatus::AVAILABLE) ||
            range.start_time > verify_before || job_imp.hasActiveJobForRange(range.range_id)) {
            continue;
        }
        ObjectReconcilePoolState &state = _object_reconcile[pool_it->id];
        if (!usable(*pool_it, state))
            continue;
        if (reconcileObjectRange(*pool_it, state, range, verify_before) == ObjectRangeCheck::ERROR)
            retry.push_back(range.range_id);
        ++checked;
    }
    if (!retry.empty()) {
        std::lock_guard<std::mutex> lk(_object_reconcile_priority_mtx);
        _object_reconcile_priority.insert(retry.begin(), retry.end());
    }

    const size_t first_pool = _object_reconcile_next_pool++ % pools.size();
    for (size_t n = 0; n < pools.size() && !exhausted(); ++n) {
        const StoragePool &pool = pools[(first_pool + n) % pools.size()];
        ObjectReconcilePoolState &state = _object_reconcile[pool.id];
        if (!usable(pool, state))
            continue;
        bool pool_done = false;
        while (!pool_done && !exhausted()) {
            auto ranges = range_imp.findPoolReconcileCandidatesBefore(
                pool.id, pool.tier, state.cursor_end, state.cursor_id, 16);
            if (ranges.empty()) {
                // End of the sweep: without a single present object the whole
                // pool looks gone, which is a misconfiguration until proven otherwise.
                if (!state.pending_all_missing.empty()) {
                    if (state.seen_present)
                        flushPendingObjectFindings(pool, state);
                    else
                        tripObjectMassLossGuard(pool, state);
                }
                state.cursor_end = std::numeric_limits<int64_t>::max();
                state.cursor_id.clear();
                state.seen_present = false;
                break;
            }
            for (const auto &range : ranges) {
                if (exhausted())
                    break;
                if (range.start_time <= verify_before && !job_imp.hasActiveJobForRange(range.range_id)) {
                    const ObjectRangeCheck check = reconcileObjectRange(pool, state, range, verify_before);
                    ++checked;
                    // Retry this range next cycle rather than skip it.
                    if (check == ObjectRangeCheck::ERROR || now < state.paused_until) {
                        pool_done = true;
                        break;
                    }
                }
                state.cursor_end = range.end_time;
                state.cursor_id = range.range_id;
            }
        }
    }
    if (checked > 0) {
        DebugL << "Object range reconciliation checked " << checked << " range(s) in "
               << budget.elapsedTime() << " ms";
    }

    GET_CONFIG(int64_t, orphan_budget_ms, Storage::kTierObjectOrphanWorkBudgetMs);
    if (orphan_budget_ms <= 0)
        return;
    Ticker orphan_budget;
    auto orphan_exhausted = [&]() {
        return _stopping || orphan_budget.elapsedTime() >= static_cast<uint64_t>(orphan_budget_ms);
    };
    for (size_t n = 0; n < pools.size() && !orphan_exhausted(); ++n) {
        const StoragePool &pool = pools[(first_pool + n) % pools.size()];
        ObjectReconcilePoolState &state = _object_reconcile[pool.id];
        if (usable(pool, state))
            runObjectOrphanSweep(pool, state, orphan_exhausted);
    }
}

void TierStorageManager::enforcePoolPressureRetention() {
    GET_CONFIG(int, max_ranges_per_cycle, Storage::kTieringMaxRangesPerCameraCycle);
    GET_CONFIG(int, delete_margin_percent, Storage::kTieringPressureDeleteMarginPercent);
    if (max_ranges_per_cycle <= 0)
        max_ranges_per_cycle = 64;
    if (delete_margin_percent < 0)
        delete_margin_percent = 0;
    const int max_pages = 16;

    std::vector<StoragePool> pools;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        for (const auto &kv : _pool_cache) {
            if (kv.second.enabled)
                pools.push_back(kv.second);
        }
    }

    // keepArchivedMinFor protects a camera's recent data from pressure deletion;
    // pressure moves still apply because they preserve the recording.
    std::unordered_map<std::string, int64_t> camera_cutoffs;
    auto deleteCutoffFor = [&](const std::string &camera_id, int64_t now) -> int64_t {
        auto it = camera_cutoffs.find(camera_id);
        if (it != camera_cutoffs.end())
            return it->second;
        int64_t cutoff = now;
        auto recorder = StatisticRecorder::Instance().getRecorder(camera_id, false);
        if (recorder) {
            auto params = recorder->getParams();
            if (!params.option.keepArchivedMinForAuto)
                cutoff = now - static_cast<int64_t>(params.option.keepArchivedMinFor);
        }
        camera_cutoffs[camera_id] = cutoff;
        return cutoff;
    };

    GET_CONFIG(int64_t, object_cooldown_seconds, Storage::kTieringObjectPressureCooldownSeconds);
    GET_CONFIG(std::string, delete_mode, Storage::kTieringPressureDeleteMode);
    const bool oldest_mode = strcasecmp(delete_mode.c_str(), "OLDEST") == 0;
    SegmentTierRangeImp range_imp;
    TieringJobImp job_imp;
    for (auto pool : pools) {
        if (_stopping)
            return;
        // The backend metric of a METRICS-mode object pool lags behind
        // deletes; wait before trusting it again to avoid overshooting.
        const bool lagging_metrics = poolTypeIsObjectStorage(pool.type) &&
                                     pool.capacity_source != kPoolCapacitySourceQuota;
        if (lagging_metrics) {
            auto cooldown = _pressure_cooldown_until.find(pool.id);
            if (cooldown != _pressure_cooldown_until.end() &&
                static_cast<int64_t>(time(nullptr)) < cooldown->second) {
                continue;
            }
        }
        // Without capacity (e.g. METRICS unavailable) total_bytes stays 0.
        fillPoolRuntimeStats(pool);
        if (pool.total_bytes <= 0 ||
            pool.usage_pct < static_cast<float>(pool.critical_watermark_percent)) {
            continue;
        }
        if (!isPoolAvailableForTierOperation(pool, false))
            continue;

        const int64_t pending_move_bytes = job_imp.sumActiveBytesBySourcePool(pool.id);
        if (pending_move_bytes > 0 &&
            pool.usage_pct < static_cast<float>(pool.critical_watermark_percent + delete_margin_percent)) {
            InfoL << "Pool " << pool.id << " is critical (" << pool.usage_pct
                  << "%) but " << pending_move_bytes
                  << " bytes are already queued to move; defer pressure deletion";
            continue;
        }

        const float target_pct = std::max(0.0f, static_cast<float>(pool.high_watermark_percent) - 5.0f);
        const int64_t target_used = static_cast<int64_t>(
            static_cast<double>(pool.total_bytes) * target_pct / 100.0);
        int64_t need = pool.used_bytes - target_used - pending_move_bytes;
        if (need <= 0)
            continue;

        const int64_t now = static_cast<int64_t>(time(nullptr));
        int64_t after_end = std::numeric_limits<int64_t>::min();
        std::string after_id;
        int deleted_count = 0;
        int64_t deleted_bytes = 0;
        if (!oldest_mode) {
            reclaimPoolBalanced(pool, now, max_pages * max_ranges_per_cycle, need,
                                deleted_count, deleted_bytes);
        }
        for (int page = 0; oldest_mode && page < max_pages && need > 0 && !_stopping; ++page) {
            auto ranges = range_imp.findDeletableByPoolAfter(
                pool.tier, pool.id, now, after_end, after_id, now, max_ranges_per_cycle);
            if (ranges.empty())
                break;
            for (const auto &range : ranges) {
                after_end = range.end_time;
                after_id = range.range_id;
                if (need <= 0 || _stopping)
                    break;
                if (range.end_time > deleteCutoffFor(range.camera_id, now))
                    continue;
                std::vector<SegmentTierRange> deletable;
                if (!carveDeletablePieces(range, deletable))
                    continue;
                for (const auto &piece : deletable) {
                    if (need <= 0 || _stopping)
                        break;
                    if (!expireRangeBestEffort(piece))
                        continue;
                    ++deleted_count;
                    const int64_t freed = std::max<int64_t>(0, piece.size_bytes);
                    deleted_bytes += freed;
                    need -= freed;
                }
            }
        }

        if (deleted_count > 0) {
            InfoL << "Pressure deletion on pool " << pool.id << " removed "
                  << deleted_count << " range(s), " << deleted_bytes << " bytes";
            if (lagging_metrics && object_cooldown_seconds > 0)
                _pressure_cooldown_until[pool.id] = now + object_cooldown_seconds;
            std::lock_guard<std::mutex> lk(_quota_usage_mtx);
            _quota_usage.erase(pool.id);
        }
        if (need > 0) {
            auto &alerted_at = _pressure_alerted_at[pool.id];
            if (now - alerted_at >= 3600) {
                alerted_at = now;
                StorageAlert alert;
                alert.id = StrUUID::make_guid(8, "alert");
                alert.level = "ERROR";
                alert.type = "POOL_CRITICAL_NO_DELETABLE";
                alert.pool_id = pool.id;
                alert.message = StrPrinter << "Pool is above its critical watermark ("
                                           << pool.usage_pct << "%) and "
                                           << need << " bytes could not be reclaimed";
                alert.created_at = now;
                StorageAlertImp().add(alert);
            }
        }
    }
}

int64_t TierStorageManager::balancedCutoff(const PressureCameraWindow &window, int keep_percent) {
    return window.hi - (window.hi - window.lo) * keep_percent / 100;
}

int TierStorageManager::findBalancedKeepPercent(const StoragePool &pool,
                                                const std::vector<PressureCameraWindow> &cameras,
                                                int64_t now,
                                                int64_t need,
                                                bool for_move,
                                                int64_t &out_estimated) {
    SegmentTierRangeImp range_imp;
    std::map<int, int64_t> freed_cache;
    auto freedAt = [&](int kp) -> int64_t {
        auto it = freed_cache.find(kp);
        if (it != freed_cache.end())
            return it->second;
        int64_t freed = 0;
        for (const auto &window : cameras) {
            freed += range_imp.sumDeletableBytesByCameraPoolEndedBy(
                window.camera_id, pool.id, pool.tier, balancedCutoff(window, kp), now, for_move);
        }
        freed_cache[kp] = freed;
        return freed;
    };

    // Largest kp whose cutoffs free `need`. Probe downward from 100 with
    // doubling steps so the common small reclaim reads little data, then
    // bisect between the last failing and the first satisfying kp.
    int ok = -1;
    int bad = 100;
    for (int step = 1; !_stopping; step *= 2) {
        const int probe = std::max(0, 100 - step);
        if (freedAt(probe) >= need) {
            ok = probe;
            break;
        }
        bad = probe;
        if (probe == 0)
            break;
    }
    while (ok >= 0 && bad - ok > 1 && !_stopping) {
        const int mid = (ok + bad) / 2;
        if (freedAt(mid) >= need)
            ok = mid;
        else
            bad = mid;
    }
    // Not even kp = 0 frees enough: take everything.
    const int keep_percent = ok >= 0 ? ok : 0;
    out_estimated = _stopping ? 0 : freedAt(keep_percent);
    return keep_percent;
}

void TierStorageManager::reclaimPoolBalanced(const StoragePool &pool,
                                             int64_t now,
                                             int max_pieces,
                                             int64_t &need,
                                             int &deleted_count,
                                             int64_t &deleted_bytes) {
    SegmentTierRangeImp range_imp;
    std::vector<PressureCameraWindow> orphans;
    std::vector<PressureCameraWindow> recorded;
    for (const auto &camera_id : range_imp.findDistinctAvailableCameras()) {
        if (_stopping)
            return;
        PressureCameraWindow window;
        window.camera_id = camera_id;
        if (!range_imp.findOldestStartByCameraPool(camera_id, pool.id, pool.tier, window.lo))
            continue;
        window.hi = now;
        auto recorder = StatisticRecorder::Instance().getRecorder(camera_id, false);
        if (recorder) {
            auto params = recorder->getParams();
            if (!params.option.keepArchivedMinForAuto)
                window.hi = now - static_cast<int64_t>(params.option.keepArchivedMinFor);
        }
        if (window.hi <= window.lo)
            continue;
        (recorder ? recorded : orphans).push_back(window);
    }

    // Data of cameras that no longer record goes before anyone's live window.
    int remaining = max_pieces;
    for (const auto *group : {&orphans, &recorded}) {
        if (need <= 0 || remaining <= 0 || _stopping)
            break;
        if (group->empty())
            continue;
        remaining -= reclaimCamerasBalanced(pool, *group, now, remaining, need,
                                            deleted_count, deleted_bytes);
    }
}

int TierStorageManager::reclaimCamerasBalanced(const StoragePool &pool,
                                                const std::vector<PressureCameraWindow> &cameras,
                                                int64_t now,
                                                int max_pieces,
                                                int64_t &need,
                                                int &deleted_count,
                                                int64_t &deleted_bytes) {
    SegmentTierRangeImp range_imp;
    int64_t estimated = 0;
    const int keep_percent = findBalancedKeepPercent(pool, cameras, now, need, false, estimated);
    if (_stopping)
        return 0;
    DebugL << "Balanced pressure deletion on pool " << pool.id << ": keep " << keep_percent
           << "% of " << cameras.size() << " camera window(s), need " << need
           << " bytes, estimated " << estimated << " bytes";

    // Round-robin one range per camera so an early stop at `need` does not
    // spare the cameras visited last.
    struct Cursor {
        const PressureCameraWindow *window;
        int64_t cutoff;
        int64_t after_end;
        std::string after_id;
        std::deque<SegmentTierRange> pending;
        bool done;
    };
    std::vector<Cursor> cursors;
    for (const auto &window : cameras) {
        Cursor cursor;
        cursor.window = &window;
        cursor.cutoff = balancedCutoff(window, keep_percent);
        cursor.after_end = std::numeric_limits<int64_t>::min();
        cursor.done = cursor.cutoff <= window.lo;
        cursors.push_back(cursor);
    }
    int processed = 0;
    bool progressed = true;
    while (need > 0 && processed < max_pieces && progressed && !_stopping) {
        progressed = false;
        for (auto &cursor : cursors) {
            if (need <= 0 || processed >= max_pieces || _stopping)
                break;
            if (cursor.done)
                continue;
            if (cursor.pending.empty()) {
                auto page = range_imp.findPoolRetentionCandidatesAfter(
                    cursor.window->camera_id, pool.id, pool.tier, cursor.cutoff,
                    cursor.after_end, cursor.after_id, now, 16);
                if (page.empty()) {
                    cursor.done = true;
                    continue;
                }
                cursor.after_end = page.back().end_time;
                cursor.after_id = page.back().range_id;
                cursor.pending.insert(cursor.pending.end(), page.begin(), page.end());
            }
            const SegmentTierRange range = cursor.pending.front();
            cursor.pending.pop_front();
            ++processed;
            progressed = true;
            SegmentTierRange piece;
            if (!splitRangeForTieringThreshold(range, cursor.cutoff, piece))
                continue;
            std::vector<SegmentTierRange> deletable;
            if (!carveDeletablePieces(piece, deletable))
                continue;
            for (const auto &item : deletable) {
                if (_stopping)
                    break;
                if (!expireRangeBestEffort(item))
                    continue;
                ++deleted_count;
                const int64_t freed = std::max<int64_t>(0, item.size_bytes);
                deleted_bytes += freed;
                need -= freed;
            }
        }
    }
    return processed;
}

bool TierStorageManager::loadEffectiveStoragePolicy(const std::string &camera_id,
                                                    StoragePolicy &out_policy,
                                                    std::string &out_error) {
    out_policy = StoragePolicy();
    out_error.clear();
    auto effective = getEffectivePolicy(camera_id);
    if (effective.policy_id.empty()) {
        out_error = "Camera has no effective storage policy";
        return false;
    }
    auto policies = getPolicy(effective.policy_id);
    if (policies.empty()) {
        out_error = "Effective storage policy not found: " + effective.policy_id;
        return false;
    }
    out_policy = policies.front();
    return true;
}

TierStorageManager::PendingJobPolicyDecision
TierStorageManager::validatePendingJobAgainstPolicy(
        const TieringJob &job,
        const SegmentTierRange *range,
        const StoragePolicy &policy,
        int64_t now,
        std::string &out_reason) const {
    out_reason.clear();
    const auto trigger = tieringJobTriggerFromString(job.job_trigger);
    // Rows created before policy context was persisted cannot be validated
    // semantically without guessing their original trigger. Preserve their
    // legacy dispatch behaviour; every newly-created job records a trigger.
    if (trigger == TieringJobTrigger::LEGACY && job.policy_id.empty())
        return PendingJobPolicyDecision::VALID;

    if (!policy.enabled) {
        out_reason = "effective policy is disabled";
        return PendingJobPolicyDecision::STALE;
    }

    auto tiers = policy.parsedTiers();
    std::sort(tiers.begin(), tiers.end(), [](const PolicyTierConfig &a,
                                             const PolicyTierConfig &b) {
        return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
    });

    const PolicyTierConfig *source_config = nullptr;
    const PolicyTierConfig *target_config = nullptr;
    for (size_t i = 0; i < tiers.size(); ++i) {
        if (!tiers[i].enabled || tiers[i].tier != job.source_tier)
            continue;
        source_config = &tiers[i];
        for (size_t j = i + 1; j < tiers.size(); ++j) {
            if (tiers[j].enabled) {
                target_config = &tiers[j];
                break;
            }
        }
        break;
    }

    const bool tier_route_matches =
        source_config && target_config &&
        source_config->overflow_action ==
            overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER) &&
        target_config->tier == job.target_tier &&
        target_config->pool_id == job.target_pool_id;

    bool retention_move_valid = false;
    bool pressure_move_valid = false;
    if (tier_route_matches) {
        const auto rules = policy.parsedAdvancedRules();
        const int64_t min_age_seconds =
            static_cast<int64_t>(rules.min_segment_age_minutes_before_move) * 60;
        int64_t retention_threshold = now -
            static_cast<int64_t>(source_config->retain_until_days) * 86400;
        if (retention_threshold > now - min_age_seconds)
            retention_threshold = now - min_age_seconds;
        retention_move_valid = job.segment_end_time <= retention_threshold;

        const int64_t pressure_threshold =
            now - std::max<int64_t>(min_age_seconds, 60);
        pressure_move_valid = rules.enable_early_move_when_pool_high &&
                              job.segment_end_time <= pressure_threshold;
    }

    const auto delete_policy = policy.parsedDeletePolicy();
    bool external_move_valid =
        delete_policy.delete_after_days > 0 &&
        delete_policy.delete_mode ==
            deleteModeToString(DeleteMode::MOVE_TO_EXTERNAL_STORAGE) &&
        delete_policy.external_pool_id == job.target_pool_id &&
        job.segment_end_time <= now -
            static_cast<int64_t>(delete_policy.delete_after_days) * 86400;
    if (external_move_valid && range && isRangeDeletionProtected(*range))
        external_move_valid = false;

    bool valid = false;
    switch (trigger) {
        case TieringJobTrigger::RETENTION:
            valid = retention_move_valid;
            break;
        case TieringJobTrigger::PRESSURE:
            valid = pressure_move_valid;
            break;
        case TieringJobTrigger::DELETE_POLICY_EXTERNAL:
            valid = external_move_valid;
            break;
        case TieringJobTrigger::LEGACY:
        default:
            valid = retention_move_valid || pressure_move_valid || external_move_valid;
            break;
    }
    if (valid)
        return PendingJobPolicyDecision::VALID;

    out_reason = StrPrinter
        << "trigger=" << tieringJobTriggerToString(trigger)
        << ", original_policy=" << (job.policy_id.empty() ? "LEGACY" : job.policy_id)
        << "@" << job.policy_updated_at
        << ", effective_policy=" << policy.id << "@" << policy.updated_at
        << ", route=" << job.source_tier << "->" << job.target_tier
        << ", target_pool=" << job.target_pool_id
        << " is no longer eligible";
    return PendingJobPolicyDecision::STALE;
}

void TierStorageManager::dispatchPendingTieringJobs() {
    if (_stopping)
        return;

    GET_CONFIG(int, max_concurrent, Storage::kTieringTransferMaxConcurrent);
    GET_CONFIG(int, max_concurrent_per_pool, Storage::kTieringTransferMaxConcurrentPerPool);
    GET_CONFIG(int, max_transcodes, Storage::kTierTranscodeMaxConcurrent);
    GET_CONFIG(bool, transcode_enabled, Storage::kTierTranscodeEnabled);
    if (max_concurrent <= 0)
        max_concurrent = 4;
    if (max_concurrent_per_pool <= 0)
        max_concurrent_per_pool = 2;
    if (max_transcodes <= 0)
        max_transcodes = 1;

    std::unordered_map<std::string, StoragePool> pool_snapshot;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        pool_snapshot = _pool_cache;
    }

    auto state = _transfer_dispatch_state;
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        if (max_concurrent - state->active_total <= 0)
            return;
    }

    struct CachedPolicyContext {
        bool attempted = false;
        bool available = false;
        StoragePolicy policy;
        std::string error;
    };
    std::unordered_map<std::string, CachedPolicyContext> policy_cache;
    std::vector<TieringJob> eligible_candidates;
    TieringJobImp job_imp;
    const int candidate_limit = std::max(max_concurrent * 32, max_concurrent);
    const int max_refill_passes = 8;
    for (int pass = 0; pass < max_refill_passes; ++pass) {
        auto candidates = job_imp.findByStatus(
            jobStatusToString(JobStatus::PENDING), candidate_limit);
        if (candidates.empty())
            break;

        std::vector<std::string> range_ids;
        range_ids.reserve(candidates.size());
        for (const auto &job : candidates) {
            if (!job.range_id.empty())
                range_ids.push_back(job.range_id);
        }
        std::unordered_map<std::string, SegmentTierRange> ranges_by_id;
        for (const auto &range : SegmentTierRangeImp().findByRangeIds(range_ids))
            ranges_by_id[range.range_id] = range;

        int removed_candidates = 0;
        for (auto job : candidates) {
            if (job.source_pool_id.empty() || job.target_pool_id.empty()) {
                if (job_imp.transitionStatus(
                    job.job_id, jobStatusToString(JobStatus::PENDING),
                    jobStatusToString(JobStatus::FAILED), -1,
                    "Tiering job has empty source_pool_id or target_pool_id")) {
                    ++removed_candidates;
                }
                continue;
            }

            auto &cached_policy = policy_cache[job.camera_id];
            if (!cached_policy.attempted) {
                cached_policy.attempted = true;
                try {
                    cached_policy.available = loadEffectiveStoragePolicy(
                        job.camera_id, cached_policy.policy, cached_policy.error);
                } catch (const std::exception &ex) {
                    cached_policy.error = ex.what();
                } catch (...) {
                    cached_policy.error = "Unknown policy lookup error";
                }
            }
            if (!cached_policy.available) {
                DebugL << "Defer tiering job because policy validation is unavailable: job="
                       << job.job_id << " camera=" << job.camera_id
                       << " error=" << cached_policy.error;
                continue;
            }

            const SegmentTierRange *range = nullptr;
            auto range_it = ranges_by_id.find(job.range_id);
            if (range_it != ranges_by_id.end())
                range = &range_it->second;
            std::string policy_reason;
            const auto policy_decision = validatePendingJobAgainstPolicy(
                job, range, cached_policy.policy,
                static_cast<int64_t>(time(nullptr)), policy_reason);
            if (policy_decision == PendingJobPolicyDecision::STALE) {
                if (job_imp.transitionStatus(
                        job.job_id, jobStatusToString(JobStatus::PENDING),
                        jobStatusToString(JobStatus::CANCELLED), job.bytes_moved,
                        "POLICY_CHANGED: " + policy_reason)) {
                    ++removed_candidates;
                    InfoL << "Cancelled stale pending tiering job: job=" << job.job_id
                          << " camera=" << job.camera_id << " reason=" << policy_reason;
                }
                continue;
            }
            if (policy_decision == PendingJobPolicyDecision::DEFER)
                continue;
            eligible_candidates.push_back(job);
        }

        if (!eligible_candidates.empty() || removed_candidates == 0)
            break;
    }

    std::vector<TieringJob> dispatch_jobs;
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        int capacity = max_concurrent - state->active_total;
        if (capacity <= 0)
            return;

        for (auto job : eligible_candidates) {
            if (capacity <= 0)
                break;
            if (job.operation == "TRANSCODE_MOVE" && !transcode_enabled)
                continue;
            auto src_it = pool_snapshot.find(job.source_pool_id);
            auto dst_it = pool_snapshot.find(job.target_pool_id);
            if (src_it == pool_snapshot.end() || dst_it == pool_snapshot.end()) {
                const std::string missing_pool = src_it == pool_snapshot.end()
                    ? job.source_pool_id : job.target_pool_id;
                job_imp.transitionStatus(
                    job.job_id, jobStatusToString(JobStatus::PENDING),
                    jobStatusToString(JobStatus::FAILED), -1,
                    "Storage pool not found: " + missing_pool);
                continue;
            }
            PoolHealthInfo source_health;
            PoolHealthInfo target_health;
            const bool source_available =
                isPoolAvailableForTierOperation(src_it->second, false, &source_health);
            const bool target_available =
                isPoolAvailableForTierOperation(dst_it->second, true, &target_health);
            if (!source_available || !target_available) {
                DebugL << "Defer tiering job because storage pool is unavailable: job="
                       << job.job_id << " source_pool=" << job.source_pool_id
                       << " source_status=" << source_health.health_status
                       << " target_pool=" << job.target_pool_id
                       << " target_status=" << target_health.health_status;
                continue;
            }
            const bool same_pool = job.source_pool_id == job.target_pool_id;
            const bool transcode = job.operation == "TRANSCODE_MOVE";
            if (transcode && state->active_transcodes >= max_transcodes)
                continue;
            if (state->active_by_pool[job.source_pool_id] >= max_concurrent_per_pool ||
                (!same_pool &&
                 state->active_by_pool[job.target_pool_id] >= max_concurrent_per_pool)) {
                continue;
            }
            if (!job_imp.transitionStatus(
                    job.job_id, jobStatusToString(JobStatus::PENDING),
                    jobStatusToString(JobStatus::RUNNING))) {
                continue;
            }
            job.status = jobStatusToString(JobStatus::RUNNING);
            ++state->active_total;
            if (transcode)
                ++state->active_transcodes;
            ++state->callbacks_in_flight;
            ++state->active_by_pool[job.source_pool_id];
            if (!same_pool)
                ++state->active_by_pool[job.target_pool_id];
            --capacity;
            dispatch_jobs.push_back(job);
        }
    }

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    for (auto job : dispatch_jobs) {
        // may_sync=false: this callback calls dispatchPendingTieringJobs() from a tierTransfer()
        // thread, which must queue the next job instead of running it inline.
        BlockingExecutors::tierTransfer()->async(
            [weak_self, state, job]() mutable {
                {
                    auto self = weak_self.lock();
                    if (self) {
                        try {
                            self->executePendingJob(job);
                        } catch (const std::exception &ex) {
                            TieringJobImp().updateStatus(
                                job.job_id, jobStatusToString(JobStatus::FAILED),
                                job.bytes_moved, ex.what());
                        } catch (...) {
                            TieringJobImp().updateStatus(
                                job.job_id, jobStatusToString(JobStatus::FAILED),
                                job.bytes_moved, "Unknown tier transfer error");
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lk(state->mutex);
                        --state->active_total;
                        if (job.operation == "TRANSCODE_MOVE" && state->active_transcodes > 0)
                            --state->active_transcodes;
                        auto release_pool = [&](const std::string &pool_id) {
                            auto it = state->active_by_pool.find(pool_id);
                            if (it == state->active_by_pool.end())
                                return;
                            if (--it->second <= 0)
                                state->active_by_pool.erase(it);
                        };
                        release_pool(job.source_pool_id);
                        if (job.target_pool_id != job.source_pool_id)
                            release_pool(job.target_pool_id);
                    }

                    if (self && !self->_stopping)
                        self->dispatchPendingTieringJobs();
                }

                // Signal stop() only after self is released; otherwise the last
                // reference could drop here and destroy the manager on this thread
                // while the process is tearing down.
                std::lock_guard<std::mutex> lk(state->mutex);
                --state->callbacks_in_flight;
                state->completed.notify_all();
            }, false);
    }
}

void TierStorageManager::executePendingJob(TieringJob &job) {
    TieringJobImp job_imp;
    if (job.source_pool_id.empty() || job.target_pool_id.empty()) {
        job_imp.updateStatus(job.job_id, jobStatusToString(JobStatus::FAILED),
                             -1, "Tiering job has empty source_pool_id or target_pool_id");
        return;
    }
    if (job.status == jobStatusToString(JobStatus::PENDING)) {
        if (!job_imp.transitionStatus(job.job_id,
                                      jobStatusToString(JobStatus::PENDING),
                                      jobStatusToString(JobStatus::RUNNING)))
            return;
        job.status = jobStatusToString(JobStatus::RUNNING);
    } else if (job.status != jobStatusToString(JobStatus::RUNNING)) {
        return;
    }

    StoragePolicy effective_policy;
    std::string policy_error;
    SegmentTierRange policy_range;
    const SegmentTierRange *policy_range_ptr = nullptr;
    PendingJobPolicyDecision policy_decision = PendingJobPolicyDecision::DEFER;
    try {
        if (loadEffectiveStoragePolicy(job.camera_id, effective_policy, policy_error)) {
            if (!job.range_id.empty()) {
                auto ranges = SegmentTierRangeImp().findByRangeIds({job.range_id});
                if (!ranges.empty()) {
                    policy_range = ranges.front();
                    policy_range_ptr = &policy_range;
                }
            }
            policy_decision = validatePendingJobAgainstPolicy(
                job, policy_range_ptr, effective_policy,
                static_cast<int64_t>(time(nullptr)), policy_error);
        }
    } catch (const std::exception &ex) {
        policy_error = ex.what();
        policy_decision = PendingJobPolicyDecision::DEFER;
    } catch (...) {
        policy_error = "Unknown policy validation error";
        policy_decision = PendingJobPolicyDecision::DEFER;
    }

    if (policy_decision == PendingJobPolicyDecision::STALE) {
        const std::string reason = "POLICY_CHANGED: " + policy_error;
        if (!job_imp.transitionStatus(
                job.job_id, jobStatusToString(JobStatus::RUNNING),
                jobStatusToString(JobStatus::CANCELLED), job.bytes_moved, reason)) {
            WarnL << "Failed to cancel stale running tiering job before I/O: " << job.job_id;
        } else {
            job.status = jobStatusToString(JobStatus::CANCELLED);
            InfoL << "Cancelled stale tiering job before I/O: job=" << job.job_id
                  << " reason=" << policy_error;
        }
        return;
    }
    if (policy_decision == PendingJobPolicyDecision::DEFER) {
        const std::string reason = "POLICY_VALIDATION_UNAVAILABLE: " + policy_error;
        if (!job_imp.transitionStatus(
                job.job_id, jobStatusToString(JobStatus::RUNNING),
                jobStatusToString(JobStatus::PENDING), job.bytes_moved, reason)) {
            WarnL << "Failed to defer tiering job after policy validation error: " << job.job_id;
        } else {
            job.status = jobStatusToString(JobStatus::PENDING);
            DebugL << reason << " job=" << job.job_id;
        }
        return;
    }

    StoragePool src_pool;
    StoragePool dst_pool;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto src_it = _pool_cache.find(job.source_pool_id);
        auto dst_it = _pool_cache.find(job.target_pool_id);
        if (src_it == _pool_cache.end() || dst_it == _pool_cache.end()) {
            const std::string missing_pool = src_it == _pool_cache.end()
                ? job.source_pool_id : job.target_pool_id;
            job_imp.updateStatus(job.job_id, jobStatusToString(JobStatus::FAILED),
                                 -1, "Storage pool not found: " + missing_pool);
            return;
        }
        src_pool = src_it->second;
        dst_pool = dst_it->second;
    }

    PoolHealthInfo source_health;
    PoolHealthInfo target_health;
    const bool source_available =
        isPoolAvailableForTierOperation(src_pool, false, &source_health);
    const bool target_available =
        isPoolAvailableForTierOperation(dst_pool, true, &target_health);
    if (!source_available || !target_available) {
        const std::string reason = StrPrinter
            << "Deferred because storage pool is unavailable: source="
            << job.source_pool_id << "(" << source_health.health_status << ")"
            << ", target=" << job.target_pool_id << "("
            << target_health.health_status << ")";
        if (!job_imp.transitionStatus(
                job.job_id, jobStatusToString(JobStatus::RUNNING),
                jobStatusToString(JobStatus::PENDING), job.bytes_moved, reason)) {
            WarnL << "Failed to defer unavailable tiering job: " << job.job_id;
        } else {
            job.status = jobStatusToString(JobStatus::PENDING);
            DebugL << reason << " job=" << job.job_id;
        }
        return;
    }

    bool ok = false;
    if (job.operation == "TRANSCODE_MOVE") {
        std::lock_guard<std::mutex> lk(_tiering_cancellation_mtx);
        _tiering_cancellations[job.job_id] =
            std::make_shared<BlockingProcessCancellation>();
    }
    if (poolTypeIsObjectStorage(dst_pool.type)) {
        ok = executeObjectStorageUpload(job, src_pool, dst_pool);
    } else {
        ok = executeLocalTierMove(job, src_pool, dst_pool);
    }
    if (job.operation == "TRANSCODE_MOVE") {
        std::lock_guard<std::mutex> lk(_tiering_cancellation_mtx);
        _tiering_cancellations.erase(job.job_id);
    }

    if (ok) {
        job_imp.updateStatus(job.job_id, jobStatusToString(JobStatus::DONE), job.bytes_moved);
        DebugL << "Tiering job done: " << job.job_id;
    } else {
        job_imp.updateStatus(job.job_id, jobStatusToString(JobStatus::FAILED),
                             job.bytes_moved, job.error_message.value_or("Move failed"));
        WarnL << "Tiering job failed: " << job.job_id;
    }
}

bool TierStorageManager::canDropMissingSegments(const TieringJob &job,
                                                const StoragePool &src_pool) const {
    if (job.range_id.empty())
        return false;
    if (countMissingSegmentFailures(job.range_id) + 1 < getMissingSegmentMaxAttempts())
        return false;
    // A missing file only proves data loss when the source pool itself is
    // reachable; an unmounted NAS also reports ENOENT for every segment.
    if (!isPoolAvailableForTierOperation(src_pool, false))
        return false;
    if (poolTypeIsObjectStorage(src_pool.type))
        return false;
    const std::string src_root = configuredPoolDataRoot(src_pool);
    struct stat root_st{};
    return !src_root.empty() && ::stat(src_root.c_str(), &root_st) == 0 &&
           S_ISDIR(root_st.st_mode);
}

void TierStorageManager::reportMissingSegments(const SegmentTierRange &range,
                                               const std::vector<TierRangeSegmentFile> &missing,
                                               const std::string &missing_from) {
    if (missing.empty())
        return;
    StorageAlert alert;
    alert.id = StrUUID::make_guid(8, "alert");
    alert.level = "WARNING";
    alert.type = "TIER_SEGMENT_MISSING";
    alert.pool_id = range.pool_id;
    alert.message = StrPrinter << "Dropped " << missing.size()
                               << " segment(s) missing from " << missing_from << ", range="
                               << range.range_id << ", first="
                               << missing.front().segment_path;
    alert.created_at = static_cast<int64_t>(time(nullptr));
    StorageAlertImp().add(alert);

    std::vector<TimeBlockIdentity> identities;
    for (const auto &segment : missing) {
        TimeBlockIdentity identity;
        identity.camera_id = segment.camera_id;
        identity.stream_id = segment.stream_id;
        identity.start_time = static_cast<uint64_t>(segment.start_time);
        identity.segment_path = segment.segment_path;
        identities.push_back(identity);
        WarnL << "Dropped missing tier segment: range=" << range.range_id
              << " segment=" << segment.segment_path;
    }
    StorageManager::Instance().removeCameraTimeBlocks(range.camera_id, identities);
}

bool TierStorageManager::commitTierTransfer(TieringJob &job,
                                            const SegmentTierRange &range,
                                            const std::vector<TierRangeSegmentFile> &segments,
                                            const std::vector<int64_t> &original_sizes,
                                            const std::vector<char> &missing,
                                            const std::string &target_tier,
                                            const std::string &target_pool_id) {
    std::vector<TierRangeSegmentFile> missing_segments;
    int64_t kept_count = 0;
    int64_t kept_size = 0;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (missing[i]) {
            missing_segments.push_back(segments[i]);
            continue;
        }
        ++kept_count;
        kept_size += std::max<int64_t>(0, segments[i].file_size);
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    bool updated = false;
    const bool all_missing = kept_count == 0;
    if (job.operation == "TRANSCODE_MOVE") {
        std::vector<TierTimefileSizeUpdate> outbox;
        for (size_t i = 0; i < segments.size(); ++i) {
            if (missing[i] || i >= original_sizes.size() ||
                segments[i].file_size == original_sizes[i])
                continue;
            TierTimefileSizeUpdate update;
            update.job_id = job.job_id;
            update.range_id = range.range_id;
            update.camera_id = segments[i].camera_id;
            update.stream_id = segments[i].stream_id;
            update.segment_path = segments[i].segment_path;
            update.start_time = segments[i].start_time;
            update.old_size = original_sizes[i];
            update.new_size = segments[i].file_size;
            update.status = "PENDING";
            update.created_at = now;
            update.updated_at = now;
            outbox.push_back(update);
        }
        updated = TierQualityTransferRepository().finalizeTransfer(
            job, range,
            all_missing ? range.tier : target_tier,
            all_missing ? range.pool_id : target_pool_id,
            all_missing ? segmentStatusToString(SegmentStatus::MISSING)
                        : segmentStatusToString(SegmentStatus::AVAILABLE),
            kept_count, kept_size, outbox, now);
    } else {
        SegmentTierRangeImp range_imp;
        if (all_missing) {
            updated = range_imp.updateStatusBySnapshotAt(
                range, segmentStatusToString(SegmentStatus::MISSING), now);
        } else if (missing_segments.empty()) {
            updated = range_imp.updateTierBySnapshotAt(
                range, target_tier, target_pool_id,
                segmentStatusToString(SegmentStatus::AVAILABLE), now);
        } else {
            updated = range_imp.updateTierBySnapshotAt(
                range, target_tier, target_pool_id,
                segmentStatusToString(SegmentStatus::AVAILABLE), now,
                kept_count, kept_size);
        }
    }
    if (!updated) {
        job.error_message = Optional<std::string>(
            "STALE_RANGE_SNAPSHOT: tier range changed during transfer");
        WarnL << job.error_message.value() << " range=" << range.range_id
              << " job=" << job.job_id;
        return false;
    }
    SegmentTierRangeImp range_imp;
    if (!all_missing && !range_imp.normalizeRangeById(range.range_id, kSegmentMergeGapSeconds, now)) {
        WarnL << "Transferred tier range but could not merge adjacent destination ranges: "
              << range.range_id;
    }
    reportMissingSegments(range, missing_segments);
    return true;
}

// ---------------------------------------------------------------------------
// Local filesystem move (LOCAL_DISK → NAS, or any mount-path–based move)
// ---------------------------------------------------------------------------
bool TierStorageManager::executeLocalTierMove(TieringJob &job,
                                               const StoragePool &src_pool,
                                               const StoragePool &dst_pool) {
    if (job.source_pool_id.empty() || src_pool.id.empty()) {
        job.error_message = Optional<std::string>("Source pool is required for local tier move");
        WarnL << job.error_message.value() << " job=" << job.job_id;
        return false;
    }
    std::string dst_base = configuredPoolDataRoot(dst_pool);
    if (dst_base.empty()) {
        WarnL << "Destination pool has no mount_path: " << dst_pool.id;
        return false;
    }
    auto dst_storage = fileStorageForPoolType(dst_pool.type);
    if (!dst_storage) {
        WarnL << "Destination pool has unsupported file storage type: " << dst_pool.type
              << " pool=" << dst_pool.id;
        return false;
    }
    if (!dst_storage->isRegistered(dst_pool.id) && !registerPoolFileStorage(dst_pool)) {
        WarnL << "Destination pool is not registered with file storage: " << dst_pool.id;
        return false;
    }

    std::string src_root = configuredPoolDataRoot(src_pool);
    if (src_root.empty()) {
        job.error_message = "Source pool has no mount_path/network_path: " + job.source_pool_id;
        WarnL << job.error_message.value();
        return false;
    }
    auto src_storage = fileStorageForPoolType(src_pool.type);
    if (!src_storage ||
        (!src_storage->isRegistered(src_pool.id) && !registerPoolFileStorage(src_pool))) {
        job.error_message = "Source pool is not registered with file storage: " + src_pool.id;
        WarnL << job.error_message.value();
        return false;
    }

    SegmentTierRange job_range;
    std::vector<TierRangeSegmentFile> segments;
    std::string collect_error;
    if (!collectSegmentFilesForJob(job, job_range, segments, collect_error)) {
        job.error_message = Optional<std::string>(collect_error);
        WarnL << collect_error << " job=" << job.job_id;
        return false;
    }

    const size_t count = segments.size();
    std::vector<int64_t> original_sizes(count, 0);
    for (size_t i = 0; i < count; ++i) original_sizes[i] = segments[i].file_size;
    std::vector<std::string> target_paths(count);
    std::vector<char> copied(count, 0);
    std::vector<char> reused(count, 0);
    std::vector<char> missing(count, 0);
    int64_t moved = 0;
    int missing_count = 0;
    bool drop_missing_checked = false;
    bool drop_missing = false;
    const bool transcode_move = job.operation == "TRANSCODE_MOVE";
    TierQualityProfile quality_profile;
    if (transcode_move) {
        Json::Value profile_json;
        Json::Reader reader;
        if (!reader.parse(job.quality_profile_json, profile_json, false)) {
            job.error_message = Optional<std::string>(
                "TIER_TRANSCODE_INVALID_PROFILE: invalid profile snapshot");
            return false;
        }
        quality_profile = TierQualityProfile::fromJson(profile_json);
    }
    FfmpegTierMediaTransformer transformer;
    Json::Value encoder_summary(Json::objectValue);
    std::map<std::string, TieringJobSegment> stored_segments;
    const std::string profile_fingerprint =
        tierQualityProfileFingerprint(job.quality_profile_json);
    if (transcode_move) {
        const std::vector<TieringJobSegment> journal_rows =
            TieringJobSegmentImp().findByJob(job.job_id);
        for (const auto &row : journal_rows) {
            if (row.status == "STORED" && row.profile_fingerprint == profile_fingerprint)
                stored_segments[tierJournalIdentity(row.stream_id, row.segment_path)] = row;
        }
    }

    auto rollback = [&](const char *reason) {
        for (size_t i = 0; i < count; ++i) {
            if (!copied[i])
                continue;
            WarnL << "Rollback copied segment after " << reason << ": pool=" << dst_pool.id
                  << " key=" << segments[i].storage_key;
            dst_storage->deleteSegment(dst_pool.id, segments[i].storage_key);
        }
    };

    for (size_t i = 0; i < count; ++i) {
        const auto &seg = segments[i];
        if (seg.full_path.empty()) {
            WarnL << "Cannot resolve source segment path from pool_id=" << job.source_pool_id
                  << " segment=" << seg.segment_path;
            rollback("source resolve failure");
            return false;
        }

        const std::string dst_path = dst_storage->resolvePath(dst_pool.id, seg.storage_key);
        if (dst_path.empty()) {
            WarnL << "Cannot resolve destination segment path pool=" << dst_pool.id
                  << " key=" << seg.storage_key;
            rollback("destination resolve failure");
            return false;
        }
        target_paths[i] = dst_path;

        if (transcode_move) {
            const auto stored = stored_segments.find(
                tierJournalIdentity(seg.stream_id, seg.segment_path));
            struct stat target_st{};
            if (stored != stored_segments.end() && stored->second.output_bytes > 0 &&
                ::stat(dst_path.c_str(), &target_st) == 0 &&
                static_cast<int64_t>(target_st.st_size) == stored->second.output_bytes) {
                reused[i] = 1;
                if (!stored->second.encoder_backend.empty())
                    segments[i].file_size = stored->second.output_bytes;
                moved += stored->second.output_bytes;
                job.output_bytes += stored->second.output_bytes;
                if (stored->second.encoder_backend.empty()) {
                    ++job.preserved_segment_count;
                } else {
                    ++job.transcoded_segment_count;
                    encoder_summary[stored->second.encoder_backend] =
                        encoder_summary.get(stored->second.encoder_backend, 0).asInt() + 1;
                }
                InfoL << "Reusing validated STORED tier target: " << dst_path;
                continue;
            }
        }

        struct stat src_st{};
        if (::stat(seg.full_path.c_str(), &src_st) != 0) {
            const int stat_errno = errno;
            if (!transcode_move && stat_errno == ENOENT &&
                dst_storage->segmentExists(dst_pool.id, seg.storage_key)) {
                moved += std::max<int64_t>(0, seg.file_size);
                InfoL << "Reusing existing tier target for missing source: " << dst_path
                      << " key=" << seg.storage_key;
                continue;
            }
            if (stat_errno == ENOENT) {
                if (!drop_missing_checked) {
                    drop_missing = canDropMissingSegments(job, src_pool);
                    drop_missing_checked = true;
                }
                if (drop_missing) {
                    missing[i] = 1;
                    ++missing_count;
                    continue;
                }
            }
            job.error_message = StrPrinter << kMissingSourceAndTargetError
                                           << ": source=" << seg.full_path
                                           << ", target=" << dst_path
                                           << ", errno=" << stat_errno;
            WarnL << job.error_message.value();
            rollback("missing source");
            return false;
        }
        if (isSameFileOnDisk(seg.full_path, dst_path)) {
            WarnL << "Refuse to move segment because source and destination are the same file: "
                  << seg.full_path << " -> " << dst_path
                  << " source_pool=" << job.source_pool_id
                  << " target_pool=" << dst_pool.id;
            rollback("same-file detection");
            return false;
        }

        std::string upload_path = seg.full_path;
        TieringJobSegment journal;
        TierTransformResult transformed;
        if (transcode_move) {
            journal.job_id = job.job_id;
            journal.range_id = job.range_id;
            journal.camera_id = seg.camera_id;
            journal.stream_id = seg.stream_id;
            journal.segment_path = seg.segment_path;
            journal.storage_key = seg.storage_key;
            journal.segment_start_time = seg.start_time;
            journal.segment_end_time = seg.end_time;
            journal.operation = "TRANSCODE";
            journal.profile_fingerprint = profile_fingerprint;
            journal.source_bytes = src_st.st_size;
            TieringJobSegmentImp journal_imp;
            journal_imp.add(journal);
            journal_imp.updateResult(journal, "TRANSFORMING", 0, "");
            TierTransformRequest transform_request;
            transform_request.job_id = job.job_id;
            transform_request.stream_id = seg.stream_id;
            transform_request.segment_path = seg.segment_path;
            transform_request.source_path = seg.full_path;
            transform_request.profile = quality_profile;
            {
                std::lock_guard<std::mutex> lk(_tiering_cancellation_mtx);
                transform_request.cancellation = _tiering_cancellations[job.job_id];
            }
            if (seg.encrypted) {
                // ffmpeg cannot read the ciphertext; an encrypted segment moves unchanged.
                transformed.success = true;
                transformed.copied_without_transcode = true;
                transformed.output_path = seg.full_path;
                transformed.output_bytes = static_cast<int64_t>(src_st.st_size);
            } else {
                transformed = transformer.transform(transform_request);
            }
            if (!transformed.success) {
                journal_imp.updateResult(journal, "FAILED", 0, "",
                                         transformed.error_code, transformed.error_message);
                job.error_message = transformed.error_code + ": " + transformed.error_message;
                rollback("transform failure");
                return false;
            }
            upload_path = transformed.output_path;
        }

        PoolTransferScope transfer(_throughput, dst_pool.id, src_pool.id);
        const bool stored = transcode_move
            ? dst_storage->uploadSegmentAtomic(dst_pool.id, upload_path, seg.storage_key, job.job_id)
            : dst_storage->uploadSegment(dst_pool.id, upload_path, seg.storage_key);
        if (!stored) {
            if (transcode_move && upload_path != seg.full_path) ::unlink(upload_path.c_str());
            if (transcode_move) {
                TieringJobSegmentImp().updateResult(
                    journal, "FAILED", 0, "", "TIER_TRANSCODE_STORE_FAILED",
                    "File storage upload failed");
            }
            WarnL << "File storage upload failed for " << upload_path
                  << " -> " << dst_path
                  << " pool=" << dst_pool.id;
            rollback("upload failure");
            return false;
        }
        transfer.finish(static_cast<uint64_t>(
            transcode_move ? transformed.output_bytes : static_cast<int64_t>(src_st.st_size)));

        copied[i] = 1;
        if (transcode_move) {
            if (!transformed.copied_without_transcode)
                segments[i].file_size = transformed.output_bytes;
            moved += transformed.output_bytes;
            job.output_bytes += transformed.output_bytes;
            if (transformed.copied_without_transcode) {
                ++job.preserved_segment_count;
            } else {
                ++job.transcoded_segment_count;
                encoder_summary[transformed.encoder_backend] =
                    encoder_summary.get(transformed.encoder_backend, 0).asInt() + 1;
                ::unlink(upload_path.c_str());
            }
            TieringJobSegmentImp().updateResult(
                journal, "STORED", transformed.output_bytes, transformed.encoder_backend);
        } else {
            moved += src_st.st_size;
        }
        InfoL << "Copied tier segment: " << seg.full_path
              << " -> " << dst_path
              << " bytes=" << src_st.st_size;
    }

    job.bytes_moved = moved;
    if (transcode_move) {
        Json::FastWriter writer;
        job.encoder_summary_json = writer.write(encoder_summary);
    }
    if (!commitTierTransfer(job, job_range, segments, original_sizes, missing,
                            job.target_tier, dst_pool.id)) {
        rollback("range update failure");
        return false;
    }

    if (transcode_move) {
        std::vector<TimeBlockSizePatch> patches;
        for (size_t i = 0; i < count; ++i) {
            if (missing[i] || segments[i].file_size == original_sizes[i])
                continue;
            TierTimefileSizeUpdate update;
            update.job_id = job.job_id;
            update.range_id = job.range_id;
            update.camera_id = segments[i].camera_id;
            update.stream_id = segments[i].stream_id;
            update.segment_path = segments[i].segment_path;
            update.start_time = segments[i].start_time;
            update.old_size = original_sizes[i];
            update.new_size = segments[i].file_size;
            TimeBlockSizePatch patch;
            patch.camera_id = update.camera_id;
            patch.stream_id = update.stream_id;
            patch.segment_path = update.segment_path;
            patch.start_time = static_cast<uint64_t>(update.start_time);
            patch.new_size = static_cast<uint64_t>(update.new_size);
            patches.push_back(patch);
        }
        StorageManager::Instance().patchCameraTimeBlockSizes(job.camera_id, patches);
    }

    for (size_t i = 0; i < count; ++i) {
        if (!copied[i] && !reused[i])
            continue;
        const auto &seg = segments[i];
        if (isSameFileOnDisk(seg.full_path, target_paths[i])) {
            WarnL << "Skip deleting source because it matches destination: "
                  << seg.full_path << " -> " << target_paths[i];
            continue;
        }
        if (::remove(seg.full_path.c_str()) != 0) {
            WarnL << "Cannot remove old source segment after successful copy: " << seg.full_path
                  << ": " << strerror(errno);
            continue;
        }
        InfoL << "Moved segment: " << seg.full_path
              << " -> " << target_paths[i]
              << " tier=" << dst_pool.tier;
    }
    pruneEmptySegmentDateDirectories(src_root, segments);
    if (missing_count > 0) {
        InfoL << "Tier move dropped " << missing_count << " missing segment(s), job="
              << job.job_id << " range=" << job_range.range_id;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Object storage upload (MINIO / S3 / ARCHIVE)
// ---------------------------------------------------------------------------
bool TierStorageManager::executeObjectStorageUpload(TieringJob &job,
                                                     const StoragePool &src_pool,
                                                     const StoragePool &dst_pool) {
    if (job.source_pool_id.empty()) {
        job.error_message = Optional<std::string>("Source pool is required for object storage upload");
        WarnL << job.error_message.value() << " job=" << job.job_id;
        return false;
    }
    if (!_obj_storage.isRegistered(dst_pool.id)) {
        job.error_message = std::string("Pool not registered with object storage client: ")
                            + dst_pool.id;
        WarnL << job.error_message.value();
        return false;
    }

    const std::string base_path = dst_pool.base_path.has_value()
                                    ? dst_pool.base_path.value() : "";
    const std::string src_root = configuredPoolDataRoot(src_pool);

    SegmentTierRange job_range;
    std::vector<TierRangeSegmentFile> segments;
    std::string collect_error;
    if (!collectSegmentFilesForJob(job, job_range, segments, collect_error)) {
        job.error_message = Optional<std::string>(collect_error);
        WarnL << "executeObjectStorageUpload: " << collect_error
              << " job=" << job.job_id;
        return false;
    }

    const size_t count = segments.size();
    std::vector<int64_t> original_sizes(count, 0);
    for (size_t i = 0; i < count; ++i) original_sizes[i] = segments[i].file_size;
    std::vector<std::string> keys(count);
    std::vector<char> uploaded(count, 0);
    std::vector<char> reused(count, 0);
    std::vector<char> missing(count, 0);
    int64_t bytes_moved = 0;
    bool drop_missing_checked = false;
    bool drop_missing = false;
    const bool transcode_move = job.operation == "TRANSCODE_MOVE";
    TierQualityProfile quality_profile;
    if (transcode_move) {
        Json::Value profile_json;
        Json::Reader reader;
        if (!reader.parse(job.quality_profile_json, profile_json, false)) {
            job.error_message = Optional<std::string>(
                "TIER_TRANSCODE_INVALID_PROFILE: invalid profile snapshot");
            return false;
        }
        quality_profile = TierQualityProfile::fromJson(profile_json);
    }
    FfmpegTierMediaTransformer transformer;
    Json::Value encoder_summary(Json::objectValue);
    std::map<std::string, TieringJobSegment> stored_segments;
    const std::string profile_fingerprint =
        tierQualityProfileFingerprint(job.quality_profile_json);
    if (transcode_move) {
        const std::vector<TieringJobSegment> journal_rows =
            TieringJobSegmentImp().findByJob(job.job_id);
        for (const auto &row : journal_rows) {
            if (row.status == "STORED" && row.profile_fingerprint == profile_fingerprint)
                stored_segments[tierJournalIdentity(row.stream_id, row.segment_path)] = row;
        }
    }

    auto rollback = [&]() {
        for (size_t i = 0; i < count; ++i) {
            if (uploaded[i])
                _obj_storage.deleteSegment(dst_pool.id, keys[i]);
        }
    };

    for (size_t i = 0; i < count; ++i) {
        const auto &seg = segments[i];
        if (seg.full_path.empty()) {
            WarnL << "executeObjectStorageUpload: cannot resolve local path for segment "
                  << seg.segment_path << " pool_id=" << job.source_pool_id;
            rollback();
            return false;
        }

        keys[i] = TierObjectStorage::makeS3Key(
            base_path, seg.camera_id, seg.stream_id, seg.segment_path);
        // An encrypted segment must keep its ".enc" spelling in the bucket, otherwise
        // the restore path downloads it and opens it as plaintext.
        keys[i] = keyForVariant(keys[i], seg.full_path);

        if (transcode_move) {
            const auto stored = stored_segments.find(
                tierJournalIdentity(seg.stream_id, seg.segment_path));
            if (stored != stored_segments.end() && stored->second.output_bytes > 0) {
                const TierObjectHeadInfo head =
                    _obj_storage.headSegmentInfo(dst_pool.id, keys[i]);
                if (head.presence == TierObjectPresence::PRESENT &&
                    head.content_length == stored->second.output_bytes) {
                    reused[i] = 1;
                    if (!stored->second.encoder_backend.empty())
                        segments[i].file_size = stored->second.output_bytes;
                    bytes_moved += stored->second.output_bytes;
                    job.output_bytes += stored->second.output_bytes;
                    if (stored->second.encoder_backend.empty()) {
                        ++job.preserved_segment_count;
                    } else {
                        ++job.transcoded_segment_count;
                        encoder_summary[stored->second.encoder_backend] =
                            encoder_summary.get(stored->second.encoder_backend, 0).asInt() + 1;
                    }
                    InfoL << "Reusing validated STORED object target key=" << keys[i];
                    continue;
                }
            }
        }

        struct stat st{};
        if (::stat(seg.full_path.c_str(), &st) != 0) {
            const int stat_errno = errno;
            if (!transcode_move && stat_errno == ENOENT &&
                _obj_storage.segmentExists(dst_pool.id, keys[i])) {
                bytes_moved += std::max<int64_t>(0, seg.file_size);
                InfoL << "executeObjectStorageUpload: reuse existing target key=" << keys[i]
                      << " for missing source=" << seg.full_path;
                continue;
            }
            if (stat_errno == ENOENT) {
                if (!drop_missing_checked) {
                    drop_missing = canDropMissingSegments(job, src_pool);
                    drop_missing_checked = true;
                }
                if (drop_missing) {
                    missing[i] = 1;
                    continue;
                }
            }
            job.error_message = StrPrinter << kMissingSourceAndTargetError
                                           << ": source=" << seg.full_path
                                           << ", target_key=" << keys[i]
                                           << ", errno=" << stat_errno;
            WarnL << "executeObjectStorageUpload: " << job.error_message.value();
            rollback();
            return false;
        }

        std::string upload_path = seg.full_path;
        TieringJobSegment journal;
        TierTransformResult transformed;
        if (transcode_move) {
            journal.job_id = job.job_id;
            journal.range_id = job.range_id;
            journal.camera_id = seg.camera_id;
            journal.stream_id = seg.stream_id;
            journal.segment_path = seg.segment_path;
            journal.storage_key = keys[i];
            journal.segment_start_time = seg.start_time;
            journal.segment_end_time = seg.end_time;
            journal.operation = "TRANSCODE";
            journal.profile_fingerprint = profile_fingerprint;
            journal.source_bytes = st.st_size;
            TieringJobSegmentImp journal_imp;
            journal_imp.add(journal);
            journal_imp.updateResult(journal, "TRANSFORMING", 0, "");
            TierTransformRequest transform_request;
            transform_request.job_id = job.job_id;
            transform_request.stream_id = seg.stream_id;
            transform_request.segment_path = seg.segment_path;
            transform_request.source_path = seg.full_path;
            transform_request.profile = quality_profile;
            {
                std::lock_guard<std::mutex> lk(_tiering_cancellation_mtx);
                transform_request.cancellation = _tiering_cancellations[job.job_id];
            }
            if (seg.encrypted) {
                // ffmpeg cannot read the ciphertext; an encrypted segment moves unchanged.
                transformed.success = true;
                transformed.copied_without_transcode = true;
                transformed.output_path = seg.full_path;
                transformed.output_bytes = static_cast<int64_t>(st.st_size);
            } else {
                transformed = transformer.transform(transform_request);
            }
            if (!transformed.success) {
                journal_imp.updateResult(journal, "FAILED", 0, "",
                                         transformed.error_code, transformed.error_message);
                job.error_message = transformed.error_code + ": " + transformed.error_message;
                rollback();
                return false;
            }
            upload_path = transformed.output_path;
        }

        PoolTransferScope transfer(_throughput, dst_pool.id, src_pool.id);
        if (!_obj_storage.uploadSegment(dst_pool.id, upload_path, keys[i])) {
            if (transcode_move && upload_path != seg.full_path) ::unlink(upload_path.c_str());
            if (transcode_move) {
                TieringJobSegmentImp().updateResult(
                    journal, "FAILED", 0, "", "TIER_TRANSCODE_STORE_FAILED",
                    "Object storage upload failed");
            }
            WarnL << "executeObjectStorageUpload: upload failed for " << upload_path;
            rollback();
            return false;
        }
        transfer.finish(static_cast<uint64_t>(
            transcode_move ? transformed.output_bytes : static_cast<int64_t>(st.st_size)));

        if (transcode_move) {
            if (!transformed.copied_without_transcode)
                segments[i].file_size = transformed.output_bytes;
            bytes_moved += transformed.output_bytes;
            job.output_bytes += transformed.output_bytes;
            if (transformed.copied_without_transcode) {
                ++job.preserved_segment_count;
            } else {
                ++job.transcoded_segment_count;
                encoder_summary[transformed.encoder_backend] =
                    encoder_summary.get(transformed.encoder_backend, 0).asInt() + 1;
                ::unlink(upload_path.c_str());
            }
            TieringJobSegmentImp().updateResult(
                journal, "STORED", transformed.output_bytes, transformed.encoder_backend);
        } else {
            bytes_moved += static_cast<int64_t>(st.st_size);
        }
        uploaded[i] = 1;
    }

    job.bytes_moved = bytes_moved;
    if (transcode_move) {
        Json::FastWriter writer;
        job.encoder_summary_json = writer.write(encoder_summary);
    }
    if (!commitTierTransfer(job, job_range, segments, original_sizes, missing,
                            tierTypeToString(tierTypeFromString(job.target_tier)),
                            dst_pool.id)) {
        rollback();
        return false;
    }

    if (transcode_move) {
        std::vector<TimeBlockSizePatch> patches;
        for (size_t i = 0; i < count; ++i) {
            if (missing[i] || segments[i].file_size == original_sizes[i])
                continue;
            TierTimefileSizeUpdate update;
            update.job_id = job.job_id;
            update.range_id = job.range_id;
            update.camera_id = segments[i].camera_id;
            update.stream_id = segments[i].stream_id;
            update.segment_path = segments[i].segment_path;
            update.start_time = segments[i].start_time;
            update.old_size = original_sizes[i];
            update.new_size = segments[i].file_size;
            TimeBlockSizePatch patch;
            patch.camera_id = update.camera_id;
            patch.stream_id = update.stream_id;
            patch.segment_path = update.segment_path;
            patch.start_time = static_cast<uint64_t>(update.start_time);
            patch.new_size = static_cast<uint64_t>(update.new_size);
            patches.push_back(patch);
        }
        StorageManager::Instance().patchCameraTimeBlockSizes(job.camera_id, patches);
    }

    for (size_t i = 0; i < count; ++i) {
        if (!uploaded[i] && !reused[i])
            continue;
        if (::unlink(segments[i].full_path.c_str()) != 0) {
            WarnL << "executeObjectStorageUpload: could not remove old local file "
                  << segments[i].full_path << ": " << strerror(errno);
        }
    }
    pruneEmptySegmentDateDirectories(src_root, segments);

    InfoL << "executeObjectStorageUpload: " << count << " segments, "
          << bytes_moved << " bytes → pool " << dst_pool.id;
    return true;
}

ColdAccessRestoreResult TierStorageManager::requestRestoreSegment(const std::string &camera_id,
                                                                  const std::string &stream_id,
                                                                  int64_t segment_start,
                                                                  const std::string &timefile_path,
                                                                  const std::string &reason,
                                                                  int64_t segment_size) {
    ColdAccessRestoreResult ret;
    std::string segment_path;
    if (camera_id.empty() || stream_id.empty() || segment_start <= 0 ||
        !deriveSegmentPathFromFullPath(timefile_path, camera_id, stream_id, segment_path))
        return ret;

    SegmentTierRangeImp range_imp;
    auto ranges = range_imp.queryByCamera(camera_id, segment_start, segment_start + 60);
    SegmentTierRange cold_range;
    bool found = false;
    for (const auto &r : ranges) {
        if (r.stream_id == stream_id &&
            r.tier == tierTypeToString(ColdTier) &&
            r.status == segmentStatusToString(SegmentStatus::AVAILABLE)) {
            cold_range = r;
            found = true;
            break;
        }
    }
    if (!found || cold_range.pool_id.empty())
        return ret;

    std::lock_guard<std::mutex> request_lk(_restore_request_mtx);
    RestoreJobImp restore_imp;
    auto active = RestoreJobSegmentImp().queryActiveSegment(camera_id, stream_id, segment_path);
    if (!active.empty()) {
        ret.handled = true;
        ret.job_id = active.front().job_id;
        ret.status = active.front().status;
        ret.message = "Restore segment already queued";
        return ret;
    }

    int64_t now = static_cast<int64_t>(time(nullptr));
    RestoreJob job;
    job.job_id = StrUUID::make_guid(8, "rj");
    job.camera_id = camera_id;
    job.source_tier = tierTypeToString(ColdTier);
    job.target_tier = tierTypeToString(HotTier);
    job.status = "PENDING";
    job.start_time = segment_start;
    job.end_time = segment_start + 60;
    const int64_t known_size = segment_size > 0
        ? segment_size
        : (cold_range.segment_count == 1 ? std::max<int64_t>(0, cold_range.size_bytes) : 0);
    job.total_bytes = known_size;
    job.processed_bytes = 0;
    job.reason = Optional<std::string>(reason);
    job.created_at = now;
    job.updated_at = now;

    int64_t segment_end = segment_start + 60;
    RestoreJobSegment child;
    child.job_id = job.job_id;
    child.camera_id = camera_id;
    child.stream_id = stream_id;
    child.range_id = cold_range.range_id;
    child.segment_path = segment_path;
    child.source_pool_id = cold_range.pool_id;
    child.range_start = segment_start;
    child.range_end = segment_end;
    child.status = "PENDING";
    child.total_bytes = known_size;
    child.created_at = now;
    child.updated_at = now;

    std::string directory_error;
    if (!prepareRestoreJobDirectory(job.job_id, directory_error)) {
        WarnL << "Cannot prepare restore segment directory job=" << job.job_id
              << ": " << directory_error;
        return ret;
    }
    try {
        if (!restore_imp.addWithSegments(job, {child})) {
            File::delete_file(buildRestoreJobRootPath(job.job_id));
            return ret;
        }
    } catch (const std::exception &ex) {
        File::delete_file(buildRestoreJobRootPath(job.job_id));
        WarnL << "Cannot enqueue restore segment: " << ex.what();
        return ret;
    } catch (...) {
        File::delete_file(buildRestoreJobRootPath(job.job_id));
        WarnL << "Cannot enqueue restore segment: unknown database error";
        return ret;
    }

    ret.handled = true;
    ret.job_created = true;
    ret.job_id = job.job_id;
    ret.status = "PENDING";
    ret.message = "Segment is in COLD tier. Restore has been queued";
    processRestoreJobs();
    return ret;
}

void TierStorageManager::requestRestoreSegmentAsync(const std::string &camera_id,
                                                    const std::string &stream_id,
                                                    int64_t segment_start_time,
                                                    const std::string &timefile_path,
                                                    const std::string &reason,
                                                    int64_t segment_size) {
    if (_stopping)
        return;
    auto state = _restore_dispatch_state;
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        ++state->tasks_in_flight;
    }
    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    BlockingExecutors::restore()->async([weak_self, state, camera_id, stream_id, segment_start_time,
                                         timefile_path, reason, segment_size]() {
        {
            auto self = weak_self.lock();
            if (self && !self->_stopping) {
                try {
                    auto restore = self->requestRestoreSegment(camera_id, stream_id, segment_start_time,
                                                               timefile_path, reason, segment_size);
                    if (!restore.handled) {
                        WarnL << "Restore was not queued, camera=" << camera_id << " stream=" << stream_id
                              << " start_time=" << segment_start_time << " message=" << restore.message;
                    }
                } catch (const std::exception &ex) {
                    WarnL << "Restore request failed, camera=" << camera_id << " stream=" << stream_id
                          << " start_time=" << segment_start_time << ": " << ex.what();
                }
            }
        }
        std::lock_guard<std::mutex> lk(state->mutex);
        --state->tasks_in_flight;
        state->completed.notify_all();
    }, false);
}

ColdAccessRestoreResult TierStorageManager::requestRestoreWindow(const std::string &camera_id,
                                                                 const std::string &stream_id,
                                                                 int64_t start_time,
                                                                 int64_t end_time,
                                                                 const std::string &reason) {
    ColdAccessRestoreResult ret;
    if (camera_id.empty() || start_time <= 0 || end_time <= start_time)
        return ret;

    RestoreJobImp restore_imp;

    SegmentTierRangeImp range_imp;
    std::vector<RestoreJobSegment> children;
    std::set<std::string> child_keys;
    int64_t total_bytes = 0;
    for (const auto &range : range_imp.queryByCamera(camera_id, start_time, end_time)) {
        if ((!stream_id.empty() && range.stream_id != stream_id) ||
            range.tier != tierTypeToString(ColdTier) ||
            range.status != segmentStatusToString(SegmentStatus::AVAILABLE) ||
            range.pool_id.empty())
            continue;
        for (const auto &file : collectSegmentFilesForRange(range)) {
            if (file.end_time <= start_time || file.start_time >= end_time)
                continue;
            if (!child_keys.insert(file.stream_id + "\n" + file.segment_path).second)
                continue;
            RestoreJobSegment child;
            child.camera_id = camera_id;
            child.stream_id = file.stream_id;
            child.range_id = range.range_id;
            child.segment_path = file.segment_path;
            child.source_pool_id = range.pool_id;
            child.range_start = file.start_time;
            child.range_end = file.end_time;
            child.status = "PENDING";
            child.total_bytes = std::max<int64_t>(0, file.file_size);
            child.created_at = static_cast<int64_t>(time(nullptr));
            child.updated_at = child.created_at;
            total_bytes += child.total_bytes;
            children.push_back(child);
        }
    }
    if (children.empty())
        return ret;

    std::lock_guard<std::mutex> request_lk(_restore_request_mtx);
    const auto requested_segments = restoreSegmentIdentities(children);
    RestoreJob active_match;
    bool has_active_match = false;
    for (const auto &candidate : restore_imp.queryOverlapping(
             camera_id, start_time, end_time, 0, stream_id)) {
        if (candidate.start_time != start_time || candidate.end_time != end_time ||
            candidate.source_tier != tierTypeToString(ColdTier) ||
            candidate.target_tier != tierTypeToString(HotTier)) {
            continue;
        }
        const auto candidate_segments = RestoreJobSegmentImp().queryByJob(candidate.job_id);
        if (candidate_segments.size() != children.size() ||
            restoreSegmentIdentities(candidate_segments) != requested_segments) {
            continue;
        }

        if (candidate.status == jobStatusToString(JobStatus::DONE)) {
            bool cache_complete = !candidate_segments.empty();
            for (const auto &segment : candidate_segments) {
                if (segment.status != jobStatusToString(JobStatus::DONE) ||
                    !File::fileExist(resolveRestoreSegmentPath(candidate, segment))) {
                    cache_complete = false;
                    break;
                }
            }
            if (!cache_complete ||
                !restore_imp.updateStatus(candidate.job_id, candidate.status,
                                          candidate.processed_bytes)) {
                continue;
            }
            ret.handled = true;
            ret.job_id = candidate.job_id;
            ret.status = candidate.status;
            ret.message = "Completed restore job reused and TTL extended";
            return ret;
        }
        if (!has_active_match &&
            (candidate.status == jobStatusToString(JobStatus::PENDING) ||
             candidate.status == jobStatusToString(JobStatus::RUNNING))) {
            active_match = candidate;
            has_active_match = true;
        }
    }
    if (has_active_match) {
        ret.handled = true;
        ret.job_id = active_match.job_id;
        ret.status = active_match.status;
        ret.message = "Equivalent restore job already queued";
        return ret;
    }

    RestoreJob job;
    job.job_id = StrUUID::make_guid(8, "rj");
    job.camera_id = camera_id;
    job.source_tier = tierTypeToString(ColdTier);
    job.target_tier = tierTypeToString(HotTier);
    job.status = "PENDING";
    job.start_time = start_time;
    job.end_time = end_time;
    job.total_bytes = total_bytes;
    job.reason = Optional<std::string>(reason);
    job.created_at = static_cast<int64_t>(time(nullptr));
    job.updated_at = job.created_at;
    for (auto &child : children)
        child.job_id = job.job_id;

    std::string directory_error;
    if (!prepareRestoreJobDirectory(job.job_id, directory_error)) {
        WarnL << "Cannot prepare restore window directory job=" << job.job_id
              << ": " << directory_error;
        return ret;
    }
    try {
        if (!restore_imp.addWithSegments(job, children)) {
            File::delete_file(buildRestoreJobRootPath(job.job_id));
            return ret;
        }
    } catch (const std::exception &ex) {
        File::delete_file(buildRestoreJobRootPath(job.job_id));
        WarnL << "Cannot enqueue restore window: " << ex.what();
        return ret;
    } catch (...) {
        File::delete_file(buildRestoreJobRootPath(job.job_id));
        WarnL << "Cannot enqueue restore window: unknown database error";
        return ret;
    }
    ret.handled = true;
    ret.job_created = true;
    ret.job_id = job.job_id;
    ret.status = job.status;
    ret.message = "Restore window queued";
    processRestoreJobs();
    return ret;
}

void TierStorageManager::processRestoreJobs() {
    if (_stopping)
        return;
    auto state = _restore_dispatch_state;
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        ++state->tasks_in_flight;
    }
    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    // Callers include media pollers (cold playback) and the API workers, so even the claim, which
    // is SQLite, runs on the restore pool.
    BlockingExecutors::restore()->async([weak_self, state]() {
        {
            auto self = weak_self.lock();
            if (self && !self->_stopping)
                self->dispatchRestoreJobs();
        }
        std::lock_guard<std::mutex> lk(state->mutex);
        --state->tasks_in_flight;
        state->completed.notify_all();
    }, false);
}

void TierStorageManager::dispatchRestoreJobs() {
    std::lock_guard<std::mutex> dispatch_lk(_restore_dispatch_mtx);
    GET_CONFIG(int, max_concurrent, Storage::kAutoRestoreMaxConcurrent);
    max_concurrent = std::max(1, max_concurrent);
    const int capacity = max_concurrent - _restore_inflight.load();
    if (capacity <= 0)
        return;
    auto state = _restore_dispatch_state;
    auto children = RestoreJobSegmentImp().claimPending(capacity);
    for (const auto &child : children) {
        ++_restore_inflight;
        RestoreJobImp().updateStatus(child.job_id, "RUNNING");
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            ++state->tasks_in_flight;
        }
        weak_ptr<TierStorageManager> weak_self = shared_from_this();
        // Never run inline: ThreadPool::async runs the task in place when called from one of its
        // own threads, and the task re-enters dispatchRestoreJobs() under _restore_dispatch_mtx.
        BlockingExecutors::restore()->async([weak_self, state, child]() {
            {
                auto self = weak_self.lock();
                if (self) {
                    struct RestoreGuard {
                        std::atomic<int> &inflight;
                        explicit RestoreGuard(std::atomic<int> &value) : inflight(value) {}
                        ~RestoreGuard() { --inflight; }
                    } guard(self->_restore_inflight);
                    if (self->_stopping) {
                        // Hand the segment back so the next start() claims it again.
                        RestoreJobSegmentImp().updateStatus(child, "PENDING", 0);
                    } else {
                        try {
                            self->executeRestoreSegment(child);
                        } catch (const std::exception &ex) {
                            RestoreJobSegmentImp().updateStatus(child, "FAILED", 0, ex.what());
                            self->updateRestoreParent(child.job_id);
                        } catch (...) {
                            RestoreJobSegmentImp().updateStatus(child, "FAILED", 0, "Unknown restore error");
                            self->updateRestoreParent(child.job_id);
                        }
                    }
                }
                if (self && !self->_stopping)
                    self->dispatchRestoreJobs();
            }
            // Signal stop() only after self is released, as the transfer callbacks do.
            std::lock_guard<std::mutex> lk(state->mutex);
            --state->tasks_in_flight;
            state->completed.notify_all();
        }, false);
    }
}

void TierStorageManager::executeRestoreSegment(const RestoreJobSegment &segment) {
    RestoreJobSegmentImp segment_imp;
    StoragePool source_pool;
    if (!getPoolById(segment.source_pool_id, source_pool) || !poolTypeIsObjectStorage(source_pool.type)) {
        segment_imp.updateStatus(segment, "FAILED", 0, "Object storage pool is unavailable");
        updateRestoreParent(segment.job_id);
        return;
    }

    if (!_obj_storage.isRegistered(source_pool.id) && !registerPoolObjStorage(source_pool)) {
        segment_imp.updateStatus(segment, "FAILED", 0, "Object storage pool is not registered");
        updateRestoreParent(segment.job_id);
        return;
    }

    std::string base_path = source_pool.base_path.value_or("");
    // An encrypted segment was uploaded under "<key>.enc"; which spelling exists is
    // decided at download time, with the plaintext key as the fallback.
    const std::string s3_key = TierObjectStorage::makeS3Key(base_path, segment.camera_id, segment.stream_id, segment.segment_path);
    std::string directory_error;
    if (!prepareRestoreJobDirectory(segment.job_id, directory_error)) {
        segment_imp.updateStatus(segment, "FAILED", 0, directory_error);
        updateRestoreParent(segment.job_id);
        return;
    }
    const std::string plain_restore_path = buildRestoreSegmentPath(
        segment.job_id, segment.camera_id, segment.stream_id, segment.segment_path);
    std::string restore_dir = plain_restore_path.substr(0, plain_restore_path.rfind('/'));
    std::string tmp_path = plain_restore_path + ".part";

    std::string mkdir_err;
    if (!ensureDirectoryPath(restore_dir, S_IRWXO | S_IRWXG | S_IRWXU, mkdir_err)) {
        segment_imp.updateStatus(segment, "FAILED", 0, mkdir_err);
        updateRestoreParent(segment.job_id);
        return;
    }

    struct stat st{};
    const std::string cached = resolveLocalVariant(plain_restore_path);
    if (regularFileExists(cached) && stat(cached.c_str(), &st) == 0) {
        ::unlink(tmp_path.c_str());
        int64_t restored_bytes = static_cast<int64_t>(st.st_size);
        segment_imp.updateStatus(segment, "DONE", restored_bytes);
        updateRestoreParent(segment.job_id);
        return;
    }

    if (::unlink(tmp_path.c_str()) != 0 && errno != ENOENT) {
        std::string err = std::string("Cannot remove stale partial restore file: ") + strerror(errno);
        segment_imp.updateStatus(segment, "FAILED", 0, err);
        updateRestoreParent(segment.job_id);
        return;
    }

    const std::string enc_s3_key = s3_key + RecordEncryptionUtils::encryptedFileSuffix();
    bool encrypted = false;
    bool ok = false;
    {
        PoolTransferScope transfer(_throughput, "", source_pool.id);
        if (s_encryption_active.load(std::memory_order_relaxed)) {
            ok = _obj_storage.downloadSegment(source_pool.id, enc_s3_key, tmp_path);
            encrypted = ok;
        }
        if (!ok)
            ok = _obj_storage.downloadSegment(source_pool.id, s3_key, tmp_path);
        if (ok)
            transfer.finish(static_cast<uint64_t>(File::fileSize(tmp_path)));
    }

    // The restored copy keeps the spelling of the object it came from, so playback
    // knows it still has to be decrypted.
    const std::string restore_path = encrypted
        ? plain_restore_path + RecordEncryptionUtils::encryptedFileSuffix()
        : plain_restore_path;

    if (!ok) {
        std::string error = "Download from object storage failed";
        // A proven loss should not wait for the sweep to reach this range. Both
        // spellings must be absent before the object counts as lost.
        const bool plain_absent = _obj_storage.headSegment(source_pool.id, s3_key) == TierObjectPresence::ABSENT;
        const bool enc_absent = !s_encryption_active.load(std::memory_order_relaxed) ||
                                _obj_storage.headSegment(source_pool.id, enc_s3_key) == TierObjectPresence::ABSENT;
        if (plain_absent && enc_absent) {
            error = "Object is missing from object storage";
            requestObjectRangeReconcile(segment.range_id);
        }
        segment_imp.updateStatus(segment, "FAILED", 0, error);
        updateRestoreParent(segment.job_id);
        ::unlink(tmp_path.c_str());
        return;
    }

    if (::rename(tmp_path.c_str(), restore_path.c_str()) != 0) {
        std::string err = std::string("Cannot move restored file: ") + strerror(errno);
        segment_imp.updateStatus(segment, "FAILED", 0, err);
        updateRestoreParent(segment.job_id);
        ::unlink(tmp_path.c_str());
        return;
    }

    int64_t restored_bytes = static_cast<int64_t>(File::fileSize(restore_path));
    segment_imp.updateStatus(segment, "DONE", restored_bytes);
    updateRestoreParent(segment.job_id);
    InfoL << "Restored segment to temp cache: " << restore_path;
}

void TierStorageManager::updateRestoreParent(const std::string &job_id) {
    auto children = RestoreJobSegmentImp().queryByJob(job_id);
    int64_t processed = 0;
    bool active = false;
    bool failed = false;
    std::string error;
    for (const auto &child : children) {
        processed += child.processed_bytes;
        active = active || child.status == "PENDING" || child.status == "RUNNING";
        if (child.status == "FAILED") {
            failed = true;
            if (error.empty()) error = child.error_message.value_or("Restore segment failed");
        }
    }
    const std::string status = active ? "RUNNING" : (failed ? "FAILED" : "DONE");
    RestoreJobImp().updateStatus(job_id, status, processed, error);
}

// ===================================================================
// Pool health monitoring
// ===================================================================
bool TierStorageManager::probePoolRuntimeStats(StoragePool &pool, std::string &out_message) {
    pool.last_health_check = static_cast<int64_t>(time(nullptr));
    pool.capacity_available = false;
    pool.capacity_status = "UNAVAILABLE";
    pool.capacity_message.clear();
    if (poolTypeIsObjectStorage(pool.type)) {
        if (!_obj_storage.isRegistered(pool.id) && pool.enabled)
            registerPoolObjStorage(pool);
        if (_obj_storage.isRegistered(pool.id)) {
            auto stats = _obj_storage.getPoolStats(pool.id);
            const bool quota_mode = pool.capacity_source == kPoolCapacitySourceQuota &&
                                    pool.capacity_quota_bytes.has_value() &&
                                    pool.capacity_quota_bytes.value() > 0;
            if (stats.is_online && quota_mode) {
                // The cluster metric includes EC parity, other buckets and lags;
                // count what this pool stores against its quota instead.
                const int64_t quota = pool.capacity_quota_bytes.value();
                double usage = static_cast<double>(quotaUsedBytes(pool.id)) * 100.0 /
                               static_cast<double>(quota);
                std::string raw_note;
                if (stats.capacity_available && stats.total_bytes > 0) {
                    const double raw_usage = static_cast<double>(stats.used_bytes) * 100.0 /
                                             static_cast<double>(stats.total_bytes);
                    // The raw metric still guards against the cluster itself filling up.
                    if (raw_usage >= static_cast<double>(pool.critical_watermark_percent) &&
                        raw_usage > usage) {
                        usage = raw_usage;
                        raw_note = StrPrinter << "; raw cluster usage " << raw_usage
                                              << "% is above the critical watermark";
                    }
                }
                pool.capacity_available = true;
                pool.capacity_status = "AVAILABLE";
                pool.total_bytes = quota;
                pool.used_bytes = static_cast<int64_t>(static_cast<double>(quota) * usage / 100.0);
                pool.usage_pct = static_cast<float>(usage);
                pool.capacity_message = StrPrinter << "Logical quota usage: " << pool.used_bytes
                                                   << " of " << quota << " bytes" << raw_note;
                if (refreshPoolVersioning(pool, false) == "Enabled")
                    pool.capacity_message += "; bucket versioning is Enabled";
                pool.health_status = healthToString(computePoolHealth(pool, pool.usage_pct));
                out_message = "Object storage probe succeeded; capacity from logical quota";
                return true;
            }
            if (stats.is_online) {
                pool.capacity_available = stats.capacity_available;
                pool.capacity_status = stats.capacity_status;
                pool.capacity_message = stats.capacity_message;
                if (refreshPoolVersioning(pool, false) == "Enabled") {
                    if (!pool.capacity_message.empty())
                        pool.capacity_message += "; ";
                    pool.capacity_message += "bucket versioning is Enabled, deleted recordings "
                                             "keep noncurrent versions until a lifecycle rule "
                                             "expires them";
                }
                if (stats.capacity_available) {
                    pool.used_bytes  = static_cast<int64_t>(stats.used_bytes);
                    pool.total_bytes = static_cast<int64_t>(stats.total_bytes);
                    pool.usage_pct   = (stats.total_bytes > 0)
                        ? static_cast<float>(stats.used_bytes) * 100.f
                          / static_cast<float>(stats.total_bytes)
                        : 0.f;
                    pool.health_status = healthToString(computePoolHealth(pool, pool.usage_pct));
                    out_message = "Object storage probe and capacity collection succeeded";
                } else {
                    pool.used_bytes = 0;
                    pool.total_bytes = 0;
                    pool.usage_pct = 0.0f;
                    pool.health_status = healthToString(StorageHealth::OK);
                    out_message = "Object storage is online; capacity is " +
                                  stats.capacity_status;
                    if (!stats.capacity_message.empty())
                        out_message += ": " + stats.capacity_message;
                }
                return true;
            }
            out_message = "Object storage connectivity probe failed";
        } else {
            out_message = "Object storage client is not registered";
        }
        pool.health_status = healthToString(StorageHealth::OFFLINE);
        return false;
    }

    auto file_storage = fileStorageForPoolType(pool.type);
    if (file_storage && !file_storage->isRegistered(pool.id) && pool.enabled)
        registerPoolFileStorage(pool);
    auto file_stats = file_storage ? file_storage->getPoolStats(pool.id) : TierPoolStats();
    if (file_stats.is_online) {
        pool.capacity_available = file_stats.capacity_available;
        pool.capacity_status = file_stats.capacity_status;
        pool.capacity_message = file_stats.capacity_message;
        pool.used_bytes  = static_cast<int64_t>(file_stats.used_bytes);
        pool.total_bytes = static_cast<int64_t>(file_stats.total_bytes);
        pool.usage_pct   = (file_stats.total_bytes > 0)
            ? static_cast<float>(file_stats.used_bytes) * 100.f
              / static_cast<float>(file_stats.total_bytes)
            : 0.f;
        pool.health_status = healthToString(computePoolHealth(pool, pool.usage_pct));
        out_message = "Filesystem storage probe succeeded";
        return true;
    }
    pool.health_status = healthToString(StorageHealth::OFFLINE);
    out_message = file_storage
        ? "Filesystem storage probe failed"
        : "Unsupported filesystem storage type";
    return false;
}

void TierStorageManager::fillPoolRuntimeStats(StoragePool &pool) {
    std::string message;
    probePoolRuntimeStats(pool, message);
}

void TierStorageManager::fillPoolEncryptionUsage(StoragePool &pool) {
    pool.encrypted_bytes = 0;
    pool.unencrypted_bytes = 0;
    pool.encrypted_pct = 0.0f;
    pool.unencrypted_pct = 0.0f;
    if (pool.id.empty())
        return;

    SegmentTierRangeImp range_imp;
    std::vector<std::string> camera_ids;
    try {
        camera_ids = range_imp.findDistinctAvailableCameras();
    } catch (const std::exception &ex) {
        WarnL << "fillPoolEncryptionUsage: cannot list cameras for pool " << pool.id << ": " << ex.what();
        return;
    } catch (...) {
        WarnL << "fillPoolEncryptionUsage: cannot list cameras for pool " << pool.id;
        return;
    }

    const std::string deleted = segmentStatusToString(SegmentStatus::DELETED);
    const std::string missing = segmentStatusToString(SegmentStatus::MISSING);
    auto &encryption = TierEncryptionManager::Instance();

    for (const auto &camera_id : camera_ids) {
        if (_stopping)
            return;

        std::vector<SegmentTierRange> ranges;
        try {
            ranges = range_imp.queryByCamera(camera_id);
        } catch (const std::exception &ex) {
            WarnL << "fillPoolEncryptionUsage: query failed for camera " << camera_id << ": " << ex.what();
            continue;
        } catch (...) {
            WarnL << "fillPoolEncryptionUsage: query failed for camera " << camera_id;
            continue;
        }

        for (const auto &r : ranges) {
            if (r.pool_id != pool.id)
                continue;
            if (r.status == deleted || r.status == missing)
                continue;
            if (r.size_bytes <= 0)
                continue;

            std::string origin_pool_id = r.origin_pool_id;
            if (origin_pool_id.empty() && r.tier == tierTypeToString(HotTier))
                origin_pool_id = r.pool_id;

            // Read from the key history, not from the files: a segment covered by a key
            // era but not yet rewritten is counted as encrypted, which is what the
            // operator asked for and what it will be within one cycle.
            if (encryption.isEncryptedAtTime(origin_pool_id, r.start_time))
                pool.encrypted_bytes += r.size_bytes;
            else
                pool.unencrypted_bytes += r.size_bytes;
        }
    }

    const int64_t total = pool.encrypted_bytes + pool.unencrypted_bytes;
    if (total > 0) {
        pool.encrypted_pct = static_cast<float>(pool.encrypted_bytes) * 100.f / static_cast<float>(total);
        pool.unencrypted_pct = 100.0f - pool.encrypted_pct;
    }
}

StorageHealth TierStorageManager::computePoolHealth(const StoragePool &pool,
                                                     float usage_pct) const {
    if (usage_pct >= static_cast<float>(pool.critical_watermark_percent))
        return StorageHealth::CRITICAL;
    if (usage_pct >= static_cast<float>(pool.high_watermark_percent))
        return StorageHealth::HIGH;
    if (usage_pct >= static_cast<float>(pool.high_watermark_percent) * 0.9f)
        return StorageHealth::WARNING;
    return StorageHealth::OK;
}

void TierStorageManager::checkAndRecordPoolHealth() {
    StoragePoolImp pool_imp;
    PoolMetricsImp metrics_imp;

    GET_CONFIG(int, failure_threshold, Storage::kPoolHealthFailureThreshold);
    GET_CONFIG(int, recovery_threshold, Storage::kPoolHealthRecoveryThreshold);
    GET_CONFIG(int64_t, metric_interval, Storage::kPoolHealthMetricSnapshotIntervalSeconds);
    if (failure_threshold <= 0) failure_threshold = 2;
    if (recovery_threshold <= 0) recovery_threshold = 2;
    if (metric_interval <= 0) metric_interval = 300;

    auto all_pools = pool_imp.queryAll("", "", "");
    int64_t now = static_cast<int64_t>(time(nullptr));

    for (auto &p : all_pools) {
        if (_stopping) break;
        if (!p.enabled || !p.health_check_enabled) continue;
        std::string probe_message;
        const bool online = probePoolRuntimeStats(p, probe_message);
        if (_stopping) break;

        PoolHealthInfo snapshot;
        bool write_metric = false;
        {
            std::lock_guard<std::mutex> lk(_pool_health_mtx);
            auto &state = _pool_health_cache[p.id];
            const std::string previous = state.info.health_status.empty()
                ? healthToString(StorageHealth::UNKNOWN)
                : state.info.health_status;
            state.info.pool_id = p.id;
            state.info.timestamp = now;
            state.info.message = probe_message;
            state.info.stale = false;
            state.info.capacity_available = p.capacity_available;
            state.info.capacity_status = p.capacity_status;
            state.info.capacity_message = p.capacity_message;

            if (online) {
                state.info.consecutive_failures = 0;
                ++state.info.consecutive_successes;
                state.info.last_success_at = now;
                if (p.capacity_available) {
                    state.info.used_bytes = p.used_bytes;
                    state.info.total_bytes = p.total_bytes;
                    state.info.usage_pct = p.usage_pct;
                }
                std::string online_status = p.health_status;
                if (!p.capacity_available &&
                    previous != healthToString(StorageHealth::UNKNOWN) &&
                    previous != healthToString(StorageHealth::OFFLINE)) {
                    online_status = previous;
                }
                if (previous != healthToString(StorageHealth::OFFLINE) ||
                    state.info.consecutive_successes >= recovery_threshold) {
                    state.info.health_status = online_status;
                } else {
                    state.info.health_status = previous;
                }
            } else {
                state.info.consecutive_successes = 0;
                ++state.info.consecutive_failures;
                if (state.info.consecutive_failures >= failure_threshold)
                    state.info.health_status = healthToString(StorageHealth::OFFLINE);
                else
                    state.info.health_status = previous;
            }

            const bool changed = previous != state.info.health_status;
            const bool first_observation = state.last_metric_at <= 0;
            write_metric = !first_observation &&
                           (changed || now - state.last_metric_at >= metric_interval);
            if (first_observation)
                state.last_metric_at = now;
            if (write_metric)
                state.last_metric_at = now;
            snapshot = state.info;
        }

        if (write_metric) {
            PoolMetrics m;
            m.pool_id       = snapshot.pool_id;
            m.timestamp     = now;
            m.used_bytes    = snapshot.used_bytes;
            m.total_bytes   = snapshot.total_bytes;
            m.usage_pct     = snapshot.usage_pct;
            m.health_status = snapshot.health_status;
            if (!metrics_imp.record(m))
                WarnL << "Failed to record pool health metric for " << m.pool_id;
        }

        DebugL << "Pool health: " << p.id << " [" << p.tier << "] "
               << snapshot.health_status << " " << snapshot.usage_pct << "%";
    }
}

void TierStorageManager::runPoolHealthCycle() {
    auto cycle_state = _health_cycle_state;
    {
        std::lock_guard<std::mutex> lk(cycle_state->mutex);
        if (_stopping || cycle_state->running) {
            DebugL << "Skip overlapping storage pool health cycle";
            return;
        }
        cycle_state->running = true;
    }

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    BlockingExecutors::tier()->async([weak_self, cycle_state]() {
        struct HealthGuard {
            std::shared_ptr<HealthCycleState> state;
            explicit HealthGuard(const std::shared_ptr<HealthCycleState> &value) : state(value) {}
            ~HealthGuard() {
                std::lock_guard<std::mutex> lk(state->mutex);
                state->running = false;
                state->completed.notify_all();
            }
        } guard(cycle_state);
        auto self = weak_self.lock();
        if (!self)
            return;
        if (self->_stopping)
            return;
        try {
            self->checkAndRecordPoolHealth();
            if (!self->_stopping)
                self->dispatchPendingTieringJobs();
        } catch (const std::exception &ex) {
            WarnL << "Storage pool health cycle failed: " << ex.what();
        } catch (...) {
            WarnL << "Storage pool health cycle failed with unknown error";
        }
    }, false);
}

bool TierStorageManager::getPoolHealth(const std::string &pool_id,
                                       PoolHealthInfo &out_health) const {
    {
        std::lock_guard<std::mutex> lk(_pool_health_mtx);
        auto it = _pool_health_cache.find(pool_id);
        if (it == _pool_health_cache.end())
            return false;
        out_health = it->second.info;
    }
    GET_CONFIG(int64_t, stale_after, Storage::kPoolHealthStaleAfterSeconds);
    if (stale_after <= 0) stale_after = 90;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    out_health.stale = out_health.timestamp <= 0 || now - out_health.timestamp > stale_after;
    return true;
}

bool TierStorageManager::isPoolAvailableForPlayback(const StoragePool &pool,
                                                     PoolHealthInfo *out_health) const {
    PoolHealthInfo health;
    health.pool_id = pool.id;
    if (!pool.health_check_enabled) {
        health.health_status = pool.health_status.empty()
            ? healthToString(StorageHealth::UNKNOWN) : pool.health_status;
        health.stale = false;
        if (out_health) *out_health = health;
        return true;
    }
    if (!pool.enabled) {
        health.health_status = healthToString(StorageHealth::OFFLINE);
        health.message = "Storage pool is disabled";
        health.stale = false;
        if (out_health) *out_health = health;
        return false;
    }
    if (!getPoolHealth(pool.id, health)) {
        health.health_status = healthToString(StorageHealth::UNKNOWN);
        health.stale = true;
        health.message = "Storage pool has not been checked";
    }
    if (out_health) *out_health = health;
    return !health.stale &&
           health.health_status != healthToString(StorageHealth::UNKNOWN) &&
           health.health_status != healthToString(StorageHealth::OFFLINE);
}

bool TierStorageManager::isPoolAvailableForTierOperation(
        const StoragePool &pool,
        bool destination_write,
        PoolHealthInfo *out_health) const {
    PoolHealthInfo health;
    health.pool_id = pool.id;
    if (!pool.enabled) {
        health.health_status = healthToString(StorageHealth::OFFLINE);
        health.message = "Storage pool is disabled";
        health.stale = false;
        if (out_health) *out_health = health;
        return false;
    }
    if (!pool.health_check_enabled) {
        health.health_status = pool.health_status.empty()
            ? healthToString(StorageHealth::UNKNOWN) : pool.health_status;
        health.stale = false;
        if (out_health) *out_health = health;
        return true;
    }
    if (!getPoolHealth(pool.id, health)) {
        health.health_status = healthToString(StorageHealth::UNKNOWN);
        health.stale = true;
        health.message = "Storage pool has not been checked";
    }
    if (out_health) *out_health = health;
    if (health.stale ||
        health.health_status == healthToString(StorageHealth::UNKNOWN) ||
        health.health_status == healthToString(StorageHealth::OFFLINE)) {
        return false;
    }
    return !destination_write ||
           health.health_status != healthToString(StorageHealth::CRITICAL);
}

void TierStorageManager::mergePoolRuntimeStats(StoragePool &pool) const {
    if (!pool.health_check_enabled)
        return;
    PoolHealthInfo health;
    if (!getPoolHealth(pool.id, health)) {
        if (pool.health_check_enabled) {
            pool.health_status = healthToString(StorageHealth::UNKNOWN);
            pool.last_health_check = 0;
        }
        return;
    }
    pool.used_bytes = health.used_bytes;
    pool.total_bytes = health.total_bytes;
    pool.usage_pct = health.usage_pct;
    pool.capacity_available = health.capacity_available;
    pool.capacity_status = health.capacity_status;
    pool.capacity_message = health.capacity_message;
    pool.health_status = health.stale
        ? healthToString(StorageHealth::UNKNOWN) : health.health_status;
    pool.last_health_check = health.timestamp;
}

// ===================================================================
// Tiering job queries
// ===================================================================
std::vector<TieringJob> TierStorageManager::listTieringJobs(const std::string &camera_id,
                                                              const std::string &status,
                                                              const std::string &source_tier,
                                                              const std::string &target_tier,
                                                              int64_t from_time, int64_t to_time,
                                                              int page, int size) {
    TieringJobImp imp;
    return imp.query(camera_id, status, source_tier, target_tier, from_time, to_time, page, size);
}

int TierStorageManager::countTieringJobs(const std::string &camera_id,
                                          const std::string &status,
                                          int64_t from_time, int64_t to_time) {
    TieringJobImp imp;
    return imp.countQuery(camera_id, status, from_time, to_time);
}

std::vector<TieringJob> TierStorageManager::getTieringJob(const std::string &job_id) {
    TieringJobImp imp;
    return imp.findByJobId(job_id);
}

bool TierStorageManager::retryTieringJob(const std::string &job_id) {
    TieringJobImp imp;
    auto jobs = imp.findByJobId(job_id);
    if (jobs.empty()) return false;
    const auto &status = jobs.front().status;
    if (status != jobStatusToString(JobStatus::FAILED) &&
        status != jobStatusToString(JobStatus::CANCELLED))
        return false;
    TieringJobSegmentImp().resetFailedForRetry(job_id);
    bool queued = imp.transitionStatus(job_id, status, jobStatusToString(JobStatus::PENDING), 0);
    if (queued)
        dispatchPendingTieringJobs();
    return queued;
}

bool TierStorageManager::cancelTieringJob(const std::string &job_id) {
    TieringJobImp imp;
    auto jobs = imp.findByJobId(job_id);
    if (jobs.empty()) return false;
    const auto &j = jobs.front();
    if (j.status != jobStatusToString(JobStatus::PENDING))
        return false;
    return imp.transitionStatus(job_id,
                                jobStatusToString(JobStatus::PENDING),
                                jobStatusToString(JobStatus::CANCELLED));
}

// ===================================================================
// Restore job queries
// ===================================================================
std::vector<RestoreJob> TierStorageManager::listRestoreJobs(const std::string &camera_id,
                                                              const std::string &status,
                                                              int64_t from_time, int64_t to_time,
                                                              int page, int size) {
    RestoreJobImp imp;
    return imp.query(camera_id, status, from_time, to_time, page, size);
}

int TierStorageManager::countRestoreJobs(const std::string &camera_id,
                                          const std::string &status,
                                          int64_t from_time, int64_t to_time) {
    RestoreJobImp imp;
    return imp.countQuery(camera_id, status, from_time, to_time);
}


std::vector<RestoreJob> TierStorageManager::getRestoreJob(const std::string &job_id) {
    RestoreJobImp imp;
    return imp.findByJobId(job_id);
}

bool TierStorageManager::approveExpiredRange(const std::string &range_id) {
    auto ranges = SegmentTierRangeImp().findByRangeIds({range_id});
    if (ranges.empty() ||
        ranges.front().status != segmentStatusToString(SegmentStatus::EXPIRED))
        return false;
    return expireRangeBestEffort(ranges.front(), segmentStatusToString(SegmentStatus::DELETED));
}

bool TierStorageManager::extendRangeRetention(const std::string &range_id,
                                              int64_t extend_until,
                                              const std::string &reason) {
    if (range_id.empty() || extend_until <= static_cast<int64_t>(time(nullptr)))
        return false;
    auto ranges = SegmentTierRangeImp().findByRangeIds({range_id});
    if (ranges.empty() || ranges.front().status == segmentStatusToString(SegmentStatus::DELETED) ||
        ranges.front().status == segmentStatusToString(SegmentStatus::MISSING))
        return false;
    SegmentRetentionOverride item;
    item.range_id = range_id;
    item.extend_until = extend_until;
    item.reason = Optional<std::string>(reason);
    item.updated_at = static_cast<int64_t>(time(nullptr));
    return SegmentRetentionOverrideImp().extendRange(item);
}

// ===================================================================
// Dashboard summary
// ===================================================================
Json::Value TierStorageManager::getDashboardSummary() {
    Json::Value v;
    auto pools = listPools();

    int64_t total_used  = 0;
    int64_t total_cap   = 0;
    std::string worst_status = healthToString(StorageHealth::OK);

    Json::Value tiers_summary(Json::objectValue);
    for (const auto &tier_name : std::vector<std::string>{"HOT", "WARM", "COLD"}) {
        tiers_summary[tier_name]["used_bytes"]  = 0;
        tiers_summary[tier_name]["total_bytes"] = 0;
        tiers_summary[tier_name]["pool_count"]  = 0;
    }
    // Average traffic (last tiering cycle) and recording rate (now) per tier, MiB/s.
    std::map<std::string, float> tier_write, tier_read, tier_ingest;
    // HOT only: the tightest pool forecast.
    double hot_remaining_days = -1;
    double hot_retention_days = -1;

    for (const auto &p : pools) {
        if (!p.enabled) continue;
        total_used += p.used_bytes;
        total_cap  += p.total_bytes;

        if (tiers_summary.isMember(p.tier)) {
            tiers_summary[p.tier]["used_bytes"]  = static_cast<Json::Int64>(
                tiers_summary[p.tier]["used_bytes"].asInt64() + p.used_bytes);
            tiers_summary[p.tier]["total_bytes"] = static_cast<Json::Int64>(
                tiers_summary[p.tier]["total_bytes"].asInt64() + p.total_bytes);
            tiers_summary[p.tier]["pool_count"]  = tiers_summary[p.tier]["pool_count"].asInt() + 1;
        }
        PoolThroughputStats throughput;
        if (getPoolThroughput(p.id, throughput)) {
            tier_write[p.tier] += throughput.avg_write_mbps;
            tier_read[p.tier] += throughput.avg_read_mbps;
        }
        tier_ingest[p.tier] += getPoolIngest(p.id).mbps;
        RetentionForecast forecast;
        if (getPoolRetentionForecast(p.id, forecast) && forecast.retention_days_estimate >= 0) {
            if (hot_remaining_days < 0 || forecast.estimated_remaining_days < hot_remaining_days)
                hot_remaining_days = forecast.estimated_remaining_days;
            if (hot_retention_days < 0 || forecast.retention_days_estimate < hot_retention_days)
                hot_retention_days = forecast.retention_days_estimate;
        }

        // Escalate overall health
        if (p.health_status == healthToString(StorageHealth::CRITICAL))
            worst_status = p.health_status;
        else if (worst_status != healthToString(StorageHealth::CRITICAL) &&
                 p.health_status == healthToString(StorageHealth::HIGH))
            worst_status = p.health_status;
        else if (worst_status == healthToString(StorageHealth::OK) &&
                 p.health_status == healthToString(StorageHealth::WARNING))
            worst_status = p.health_status;
    }

    v["total_used_bytes"]  = static_cast<Json::Int64>(total_used);
    v["total_bytes"]       = static_cast<Json::Int64>(total_cap);
    v["usage_pct"]         = (total_cap > 0) ? (100.0f * static_cast<float>(total_used) / static_cast<float>(total_cap)) : 0.0f;
    v["overall_health"]    = worst_status;
    v["pool_count"]        = static_cast<int>(pools.size());
    auto round2 = [](double x) { return std::round(x * 100.0) / 100.0; };
    for (const auto &tier_name : std::vector<std::string>{"HOT", "WARM", "COLD"}) {
        tiers_summary[tier_name]["write_mbps"] = round2(tier_write[tier_name]);
        tiers_summary[tier_name]["read_mbps"] = round2(tier_read[tier_name]);
        tiers_summary[tier_name]["record_ingest_mbps"] = round2(tier_ingest[tier_name]);
    }
    tiers_summary["HOT"]["estimated_remaining_days"] =
        hot_remaining_days >= 0 ? Json::Value(round2(hot_remaining_days)) : Json::Value(Json::nullValue);
    tiers_summary["HOT"]["retention_days_estimate"] =
        hot_retention_days >= 0 ? Json::Value(round2(hot_retention_days)) : Json::Value(Json::nullValue);
    v["tiers"]             = tiers_summary;

    TieringJobImp job_imp;
    v["pending_jobs"] = job_imp.countQuery("", jobStatusToString(JobStatus::PENDING));
    v["running_jobs"] = job_imp.countQuery("", jobStatusToString(JobStatus::RUNNING));

    return v;
}

Json::Value TierStorageManager::getDashboardDetail() {
    Json::Value data;
    data["summary"] = getDashboardSummary();

    auto pools = listPools();
    Json::Value pools_arr(Json::arrayValue);
    for (const auto &p : pools) {
        auto pv = p.toJson();
        // Same object as pool/list and pool/detail; null before the first tiering cycle.
        PoolThroughputStats throughput;
        pv["throughput"] = getPoolThroughput(p.id, throughput) ? throughput.toJson() : Json::Value(Json::nullValue);
        if (p.tier == tierTypeToString(HotTier)) {
            RetentionForecast forecast;
            pv["retention_forecast"] = getPoolRetentionForecast(p.id, forecast) ? forecast.toJson()
                                                                               : Json::Value(Json::nullValue);
        }
        pools_arr.append(pv);
    }
    data["pools"] = pools_arr;

    auto policies = listPolicies("", -1, 0, 1000);
    PolicyAssignmentImp assignment_imp;

    Json::Value policies_arr(Json::arrayValue);
    std::unordered_map<std::string, StoragePolicy> policy_map;
    for (const auto &p : policies) {
        policy_map[p.id] = p;
        auto pv = p.toJson();
        pv["camera_count"] = static_cast<int>(assignment_imp.findCamerasByPolicyId(p.id).size());
        policies_arr.append(pv);
    }
    data["policies"] = policies_arr;

    Json::Value cameras_arr(Json::arrayValue);
    auto assignments = assignment_imp.findAll();
    for (const auto &a : assignments) {
        Json::Value cv;
        cv["camera_id"] = a.camera_id;
        cv["policy_id"] = a.policy_id;
        cv["override_reason"] = a.override_reason.value_or("");
        cv["assigned_at"] = static_cast<Json::Int64>(a.assigned_at);

        auto pit = policy_map.find(a.policy_id);
        cv["policy_name"] = pit != policy_map.end() ? pit->second.name : "";
        cv["policy_enabled"] = pit != policy_map.end() ? (pit->second.enabled != 0) : false;

        auto recorder = StatisticRecorder::Instance().getRecorder(a.camera_id, false);
        if (recorder) {
            auto params = recorder->getParams();
            cv["camera_name"] = params.option.name;
            cv["camera_ip"] = params.option.ip;
        } else {
            cv["camera_name"] = "";
            cv["camera_ip"] = "";
        }
        cameras_arr.append(cv);
    }
    data["configured_cameras"] = cameras_arr;

    TieringJobImp job_imp;
    Json::Value job_counts(Json::objectValue);
    for (const auto &status : std::vector<std::string>{"PENDING", "RUNNING", "DONE", "FAILED", "CANCELLED"}) {
        job_counts[status] = job_imp.countQuery("", status);
    }
    data["job_counts"] = job_counts;

    Json::Value jobs_arr(Json::arrayValue);
    auto jobs = listTieringJobs("", "", "", "", 0, 0, 0, 20);
    for (const auto &j : jobs) {
        jobs_arr.append(j.toJson());
    }
    data["recent_jobs"] = jobs_arr;

    return data;
}

// ===================================================================
// Helpers
// ===================================================================
bool TierStorageManager::getPoolThroughput(const std::string &pool_id, PoolThroughputStats &out) const {
    return _throughput.getStats(pool_id, out);
}

PoolIngestRates TierStorageManager::getPoolIngest(const std::string &pool_id) {
    GET_CONFIG(int, window_sec, Storage::kPoolThroughputWindowSeconds);
    return _throughput.ingestRates(pool_id, window_sec, steadyNowMs());
}

bool TierStorageManager::getPoolRetentionForecast(const std::string &pool_id, RetentionForecast &out) const {
    return _hot_forecast.get(pool_id, out);
}

Json::Value TierStorageManager::makeHotWriteJson() {
    std::vector<StoragePool> hot_pools;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        for (const auto &kv : _pool_cache) {
            if (kv.second.enabled && kv.second.tier == tierTypeToString(HotTier))
                hot_pools.push_back(kv.second);
        }
    }
    std::sort(hot_pools.begin(), hot_pools.end(), [](const StoragePool &a, const StoragePool &b) {
        return a.name < b.name;
    });

    Json::Value pools(Json::arrayValue);
    std::set<std::string> cameras;
    float total_mbps = 0.0f;
    bool valid = false;
    bool low = false;
    for (const auto &pool : hot_pools) {
        auto rates = getPoolIngest(pool.id);
        Json::Value item;
        item["pool_id"] = pool.id;
        item["pool_name"] = pool.name;
        item["ingest_mbps"] = rates.valid ? Json::Value(sanitize_for_json(rates.mbps)) : Json::Value(Json::nullValue);
        item["recording_cameras"] = static_cast<int>(rates.camera_mbps.size());
        RetentionForecast forecast;
        if (getPoolRetentionForecast(pool.id, forecast)) {
            item["retention_forecast"] = forecast.toJson();
            low = low || forecast.level > 0;
        } else {
            item["retention_forecast"] = Json::nullValue;
        }
        pools.append(item);
        valid = valid || rates.valid;
        total_mbps += rates.mbps;
        for (const auto &cam : rates.camera_mbps)
            cameras.insert(cam.first);
    }

    Json::Value val;
    val["mode"] = "hot_tier";
    val["ingest_mbps"] = valid ? Json::Value(sanitize_for_json(total_mbps)) : Json::Value(Json::nullValue);
    val["recording_cameras"] = static_cast<int>(cameras.size());
    val["state"] = low ? "low" : "normal";
    val["pools"] = pools;
    return val;
}

void TierStorageManager::getStorageUsage(int64_t &used_bytes, int64_t &total_bytes,
                                         int64_t &padding_bytes) const {
    used_bytes = 0;
    total_bytes = 0;
    padding_bytes = 0;
    std::vector<StoragePool> pools;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        for (const auto &kv : _pool_cache) {
            if (kv.second.enabled)
                pools.push_back(kv.second);
        }
    }
    for (auto &pool : pools) {
        mergePoolRuntimeStats(pool);
        int64_t capacity = 0;
        if (poolTypeIsObjectStorage(pool.type)) {
            if (pool.capacity_source == kPoolCapacitySourceQuota && pool.capacity_quota_bytes.has_value())
                capacity = pool.capacity_quota_bytes.value();
        } else if (pool.capacity_available) {
            capacity = pool.total_bytes;
        }
        if (capacity <= 0)
            continue;
        if (pool.capacity_available)
            used_bytes += pool.used_bytes;
        total_bytes += capacity;
        const int critical = std::min(100, std::max(0, pool.critical_watermark_percent));
        padding_bytes += static_cast<int64_t>(static_cast<double>(capacity) * (100 - critical) / 100.0);
    }
}

void TierStorageManager::evaluateRetentionForecast(
        const std::vector<std::pair<std::string, StoragePolicy>> &camera_policies) {
    GET_CONFIG(bool, alert_enabled, Storage::kRetentionForecastAlertEnabled);
    GET_CONFIG(float, safety_factor, Storage::kRetentionForecastSafetyFactor);
    GET_CONFIG(int, sustain_samples, Storage::kRetentionForecastSustainSamples);
    GET_CONFIG(int, repeat_sec, Storage::kRetentionForecastAlertRepeatSeconds);

    // HOT tier of each camera's effective policy.
    std::unordered_map<std::string, PolicyTierConfig> hot_tiers;
    for (const auto &item : camera_policies) {
        if (item.second.enabled == 0)
            continue;
        for (const auto &t : item.second.parsedTiers()) {
            if (t.enabled && t.tier == tierTypeToString(HotTier)) {
                hot_tiers[item.first] = t;
                break;
            }
        }
    }

    const std::string move_action = overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER);
    for (const auto &pool : listPools(tierTypeToString(HotTier))) {
        if (!pool.enabled)
            continue;
        const int64_t now_ms = steadyNowMs();
        auto rates = getPoolIngest(pool.id);

        RetentionForecastInput in;
        // Unknown capacity (object storage without a quota, unreachable mount) gives no forecast.
        in.ingest_valid = rates.valid && pool.total_bytes > 0;
        in.ingest_mbps = rates.mbps;
        in.total_bytes = pool.total_bytes;
        in.used_bytes = pool.used_bytes;
        in.usable_percent = pool.high_watermark_percent;
        // Moved early to the next tier is a warning; deleted early loses recordings.
        in.short_level = 1;
        std::string overflow_action = move_action;
        for (const auto &cam : rates.camera_mbps) {
            RetentionCameraDemand demand;
            demand.camera_id = cam.first;
            demand.mbps = cam.second;
            auto it = hot_tiers.find(cam.first);
            if (it != hot_tiers.end() && it->second.retain_until_days > 0) {
                demand.required_sec = static_cast<int64_t>(it->second.retain_until_days) * 86400;
                if (it->second.overflow_action != move_action) {
                    in.short_level = 2;
                    overflow_action = it->second.overflow_action;
                }
            }
            in.cameras.push_back(demand);
        }

        auto forecast = computeRetentionForecast(in, safety_factor);
        const bool emit = _hot_forecast.update(pool.id, forecast, sustain_samples,
                                               static_cast<int64_t>(repeat_sec) * 1000, now_ms);
        if (!emit || !alert_enabled)
            continue;

        if (forecast.level == 0) {
            _retention_alerted_at.erase(pool.id);
            InfoL << "HOT pool " << pool.id << " holds the configured retention again ("
                  << format_float_2f(static_cast<float>(forecast.retention_days_estimate)) << " day(s))";
        } else {
            const std::string message = StrPrinter
                << "At " << format_float_2f(forecast.ingest_mbps) << " MiB/s the pool holds "
                << format_float_2f(static_cast<float>(forecast.retention_days_estimate))
                << " day(s) below its high watermark, short of the "
                << format_float_2f(static_cast<float>(forecast.required_days))
                << " day(s) the HOT retention of " << forecast.cameras << " camera(s) needs ("
                << forecast.required_bytes << " of " << forecast.usable_bytes << " bytes); overflow action "
                << overflow_action;
            WarnL << "HOT pool " << pool.id << ": " << message;
            const int64_t now = static_cast<int64_t>(time(nullptr));
            // One row an hour per pool and level: a move to another level is always recorded.
            auto &alerted = _retention_alerted_at[pool.id];
            if (alerted.second != forecast.level || now - alerted.first >= 3600) {
                alerted = std::make_pair(now, forecast.level);
                StorageAlert item;
                item.id = StrUUID::make_guid(8, "alert");
                item.level = forecast.level > 1 ? "CRITICAL" : "WARNING";
                item.type = "TIER_RETENTION_CAPACITY_LOW";
                item.pool_id = pool.id;
                item.message = message;
                item.created_at = now;
                StorageAlertImp().add(item);
            }
        }
        const std::string scope = "hot_pool";
        const std::string path = pool.mount_path.value_or("");
        const std::string mount_point;
        const int level = forecast.level;
        const double retention_days = forecast.retention_days_estimate;
        const double required_days = forecast.required_days;
        const float ingest_mbps = forecast.ingest_mbps;
        const int64_t usable_bytes = forecast.usable_bytes;
        const int64_t required_bytes = forecast.required_bytes;
        const int cameras = forecast.cameras;
        if (!NOTICE_EMIT(BroadcastRetentionForecastAlertArgs, Broadcast::kBroadcastRetentionForecastAlert, scope,
                         pool.id, pool.name, path, mount_point, level, retention_days, required_days, ingest_mbps,
                         usable_bytes, required_bytes, cameras, overflow_action)) {
            TraceL << "No one listen retention forecast alert event";
        }
    }
}

void TierStorageManager::evaluatePoolThroughput(
        const std::vector<std::pair<std::string, StoragePolicy>> &camera_policies) {
    // Every tier that moves data on to the next enabled tier routes the camera's recordings
    // into that tier's pool, so a HOT -> WARM -> COLD chain counts the camera on both targets.
    std::map<std::string, std::set<std::string>> routes;
    for (const auto &item : camera_policies) {
        const auto &policy = item.second;
        if (policy.enabled == 0)
            continue;
        auto tiers = policy.parsedTiers();
        std::sort(tiers.begin(), tiers.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
            return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
        });
        for (size_t i = 0; i < tiers.size(); ++i) {
            if (!tiers[i].enabled || tiers[i].pool_id.empty() ||
                tiers[i].overflow_action != overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER))
                continue;
            for (size_t j = i + 1; j < tiers.size(); ++j) {
                if (!tiers[j].enabled)
                    continue;
                if (!tiers[j].pool_id.empty() && tiers[j].pool_id != tiers[i].pool_id)
                    routes[tiers[j].pool_id].insert(item.first);
                break;
            }
        }
    }
    _throughput.setRoutes(routes);

    GET_CONFIG(bool, alert_enabled, Storage::kPoolThroughputAlertEnabled);
    GET_CONFIG(int, window_sec, Storage::kPoolThroughputWindowSeconds);
    GET_CONFIG(float, safety_factor, Storage::kPoolThroughputSafetyFactor);
    GET_CONFIG(int, min_busy_sec, Storage::kPoolThroughputMinBusySeconds);
    GET_CONFIG(int, sustain_samples, Storage::kPoolThroughputSustainSamples);
    GET_CONFIG(int, repeat_sec, Storage::kPoolThroughputAlertRepeatSeconds);
    PoolThroughputConfig cfg;
    cfg.window_sec = window_sec > 0 ? window_sec : 3600;
    cfg.safety_factor = safety_factor >= 1.0f ? safety_factor : 1.0f;
    cfg.min_busy_sec = min_busy_sec > 0 ? min_busy_sec : 60;
    cfg.sustain_samples = sustain_samples > 0 ? sustain_samples : 1;
    cfg.alert_repeat_sec = repeat_sec;

    const auto backlog = TieringJobImp().sumActiveBytesByTargetPool();
    const auto alerts = _throughput.evaluate(backlog, cfg, steadyNowMs());
    if (!alert_enabled)
        return;

    for (const auto &alert : alerts) {
        const auto &st = alert.stats;
        StoragePool pool;
        if (!getPoolById(st.pool_id, pool))
            continue;
        if (!st.low) {
            _throughput_alerted_at.erase(st.pool_id);
            InfoL << "Pool " << st.pool_id << " transfer throughput recovered: "
                  << format_float_2f(st.write_mbps) << " MiB/s, demand "
                  << format_float_2f(st.demand_mbps) << " MiB/s";
        } else {
            const std::string message = StrPrinter
                << "Tier transfers into the pool run at " << format_float_2f(st.write_mbps)
                << " MiB/s, below the " << format_float_2f(alert.required_mbps)
                << " MiB/s needed for the recording rate of " << st.demand_cameras
                << " camera(s) (" << format_float_2f(st.demand_mbps) << " MiB/s); backlog "
                << st.backlog_bytes << " bytes"
                << (st.backlog_eta_sec >= 0 ? ", about " + std::to_string(st.backlog_eta_sec) + " s to drain" : "");
            WarnL << "Pool " << st.pool_id << ": " << message;
            const int64_t now = static_cast<int64_t>(time(nullptr));
            auto &alerted_at = _throughput_alerted_at[st.pool_id];
            if (now - alerted_at >= 3600) {
                alerted_at = now;
                StorageAlert item;
                item.id = StrUUID::make_guid(8, "alert");
                item.level = "WARNING";
                item.type = "TIER_POOL_THROUGHPUT_LOW";
                item.pool_id = st.pool_id;
                item.message = message;
                item.created_at = now;
                StorageAlertImp().add(item);
            }
        }
        const float capacity = st.write_mbps;
        const float demand = st.demand_mbps;
        const float required = alert.required_mbps;
        const int64_t backlog_bytes = st.backlog_bytes;
        const int64_t eta_sec = st.backlog_eta_sec;
        const bool is_low = st.low;
        if (!NOTICE_EMIT(BroadcastPoolThroughputAlertArgs, Broadcast::kBroadcastPoolThroughputAlert, pool.id,
                         pool.name, pool.tier, capacity, demand, required, backlog_bytes, eta_sec, is_low)) {
            TraceL << "No one listen pool throughput alert event";
        }
    }
}

void TierStorageManager::pruneOldMetrics() {
    // Keep 30 days of metrics
    int64_t cutoff = static_cast<int64_t>(time(nullptr)) - 30LL * 86400;
    PoolMetricsImp imp;
    imp.pruneOlderThan(cutoff);

    GET_CONFIG(int64_t, tiering_job_retention_days, Storage::kTieringJobRetentionDays);
    if (tiering_job_retention_days > 0) {
        const int64_t job_cutoff = static_cast<int64_t>(time(nullptr)) - tiering_job_retention_days * 86400;
        TieringJobImp().pruneTerminalOlderThan(job_cutoff);
        RestoreJobImp().pruneExpiredOlderThan(job_cutoff);
    }

    GET_CONFIG(int64_t, alert_retention_days, Storage::kStorageAlertRetentionDays);
    if (alert_retention_days > 0) {
        StorageAlertImp().pruneOlderThan(static_cast<int64_t>(time(nullptr)) - alert_retention_days * 86400);
    }

    GET_CONFIG(int64_t, terminal_range_retention_days, Storage::kTieringTerminalRangeRetentionDays);
    if (terminal_range_retention_days > 0) {
        auto terminal_cutoff = static_cast<int64_t>(time(nullptr)) - terminal_range_retention_days * 86400;
        SegmentTierRangeImp().pruneDeletedOlderThan(terminal_cutoff);
        TierMetadataCleanupJobImp().pruneDoneOlderThan(terminal_cutoff);
    }

    // Status gaps live as long as the camera's recordings: cut them at its oldest stored range.
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (now - _last_gap_retention_at >= 3600) {
        _last_gap_retention_at = now;
        std::unordered_map<std::string, uint64_t> camera_oldest;
        auto oldest_per_stream = SegmentTierRangeImp().findOldestStoredStartPerStream();
        for (const auto &r : oldest_per_stream) {
            if (r.start_time <= 0) continue;
            auto &oldest = camera_oldest[r.camera_id];
            if (!oldest || oldest > static_cast<uint64_t>(r.start_time))
                oldest = static_cast<uint64_t>(r.start_time);
        }
        std::unordered_map<std::string, uint64_t> cutoffs(camera_oldest);
        for (const auto &r : oldest_per_stream) {
            auto it = camera_oldest.find(r.camera_id);
            if (it != camera_oldest.end() && !r.stream_id.empty())
                cutoffs[r.stream_id] = it->second;
        }
        ResourceStatusManager::Instance().setRetentionCutoffs(std::move(cutoffs));
    }
}

void TierStorageManager::cleanupRestoreTempFiles() {
    GET_CONFIG(int64_t, restore_ttl_seconds, Storage::kRestoreTTLSeconds);
    if (restore_ttl_seconds < 0)
        return;

    std::string restore_root = getRestoreRootPath();
    if (restore_root.empty() || !File::is_dir(restore_root))
        return;

    std::lock_guard<std::mutex> request_lk(_restore_request_mtx);

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int64_t cutoff = now - restore_ttl_seconds;
    RestoreJobImp restore_imp;

    int deleted_count = 0;
    int expired_job_count = 0;
    auto expired_jobs = restore_imp.queryExpiredTerminal(cutoff);
    for (const auto &job : expired_jobs) {
        if (_restore_cache_leases.count(job.job_id))
            continue;
        bool cleanup_succeeded = true;
        const auto job_root = buildRestoreJobRootPath(job.job_id);
        if (job_root.empty()) {
            WarnL << "Cannot clean restore job with unsafe id=" << job.job_id;
            cleanup_succeeded = false;
        } else if (isJobScopedRestoreLayout(job.job_id)) {
            const int result = File::delete_file(job_root, false, false);
            struct stat remaining_path{};
            const bool path_exists = ::lstat(job_root.c_str(), &remaining_path) == 0;
            if (result == 0 || !path_exists) {
                ++deleted_count;
            } else {
                WarnL << "Cannot remove restore job directory job=" << job.job_id
                      << " path=" << job_root << ": " << strerror(errno);
                cleanup_succeeded = false;
            }
        } else {
            // Transitional cleanup for jobs created with the legacy shared layout.
            auto segments = RestoreJobSegmentImp().queryByJob(job.job_id);
            for (const auto &seg : segments) {
                auto path = buildLegacyRestoreSegmentPath(
                    seg.camera_id, seg.stream_id, seg.segment_path);
                if (::unlink(path.c_str()) == 0) {
                    ++deleted_count;
                } else if (errno != ENOENT && errno != ENOTDIR) {
                    WarnL << "Cannot remove legacy restore cache file job=" << job.job_id
                          << " path=" << path << ": " << strerror(errno);
                    cleanup_succeeded = false;
                }
                std::string tmp_path = path + ".restore." + job.job_id;
                if (::unlink(tmp_path.c_str()) == 0) {
                    ++deleted_count;
                } else if (errno != ENOENT && errno != ENOTDIR) {
                    WarnL << "Cannot remove legacy temporary restore file job=" << job.job_id
                          << " path=" << tmp_path << ": " << strerror(errno);
                    cleanup_succeeded = false;
                }
            }
        }

        if (cleanup_succeeded) {
            if (restore_imp.updateStatus(job.job_id, "EXPIRED", job.processed_bytes)) {
                ++expired_job_count;
            } else {
                WarnL << "Cannot mark cleaned restore job expired job=" << job.job_id;
            }
        }
    }

    // Clean up abandoned partial downloads for failed/interrupted jobs.
    File::scanDir(restore_root, [&](const std::string &path, bool isDir) {
        if (isDir || path.find(".restore.") == std::string::npos)
            return true;
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 &&
            now - static_cast<int64_t>(st.st_mtime) > restore_ttl_seconds) {
            if (::unlink(path.c_str()) != 0 && errno != ENOENT && errno != ENOTDIR) {
                WarnL << "Cannot remove abandoned restore file path=" << path << ": " << strerror(errno);
            }
        }
        return true;
    }, false, true);

    if (expired_job_count > 0 || deleted_count > 0) {
        DebugL << "cleanupRestoreTempFiles: expired_jobs=" << expired_job_count
               << ", deleted_files=" << deleted_count
               << ", root=" << restore_root;
    }
}

void TierStorageManager::cleanupOrphanTemporarySegments() {
    GET_CONFIG(bool, cleanup_enabled, Storage::kTierTmpCleanupEnabled);
    if (!cleanup_enabled) {
        std::lock_guard<std::mutex> lk(_temporary_mp4_observations_mtx);
        _temporary_mp4_observations.clear();
        _temporary_mp4_next_pool_index = 0;
        _temporary_mp4_next_camera_index = 0;
        return;
    }

    GET_CONFIG(bool, dry_run, Storage::kTierTmpCleanupDryRun);
    GET_CONFIG(int, max_files_per_cycle, Storage::kTierTmpCleanupMaxFilesPerCycle);
    GET_CONFIG(int64_t, work_budget_ms, Storage::kTierTmpCleanupWorkBudgetMs);
    if (max_files_per_cycle <= 0)
        max_files_per_cycle = 1000;
    if (work_budget_ms <= 0)
        work_budget_ms = 500;

    std::vector<StoragePool> pools;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        pools.reserve(_pool_cache.size());
        for (const auto &kv : _pool_cache) {
            const auto &pool = kv.second;
            if (pool.enabled &&
                pool.tier == tierTypeToString(HotTier) &&
                !poolTypeIsObjectStorage(pool.type)) {
                pools.push_back(pool);
            }
        }
    }

    std::vector<std::string> roots;
    roots.reserve(pools.size());
    for (const auto &pool : pools) {
        PoolHealthInfo health;
        if (!isPoolAvailableForTierOperation(pool, false, &health)) {
            DebugL << "Skip orphan MP4 cleanup because HOT pool is unavailable: pool="
                   << pool.id << " status=" << health.health_status
                   << " stale=" << health.stale;
            continue;
        }
        std::string root = normalizeStoragePath(configuredPoolDataRoot(pool));
        struct stat root_stat{};
        if (root.empty() || root == "/" ||
            ::lstat(root.c_str(), &root_stat) != 0 ||
            !S_ISDIR(root_stat.st_mode)) {
            continue;
        }
        roots.push_back(root);
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

    std::unordered_map<std::string, TemporaryMp4Observation> previous_observations;
    size_t start_pool_index = 0;
    size_t start_camera_index = 0;
    {
        std::lock_guard<std::mutex> lk(_temporary_mp4_observations_mtx);
        previous_observations = _temporary_mp4_observations;
        if (!roots.empty())
            start_pool_index = _temporary_mp4_next_pool_index % roots.size();
        start_camera_index = _temporary_mp4_next_camera_index;
    }

    if (roots.empty()) {
        std::lock_guard<std::mutex> lk(_temporary_mp4_observations_mtx);
        _temporary_mp4_observations.clear();
        _temporary_mp4_next_pool_index = 0;
        _temporary_mp4_next_camera_index = 0;
        return;
    }

    std::unordered_map<std::string, TemporaryMp4Observation> current_observations;
    std::unordered_set<std::string> seen_candidates;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    struct CameraCleanupRetention {
        bool valid = false;
        uint64_t cutoff_date = 0;
    };
    std::unordered_map<std::string, CameraCleanupRetention> retention_cache;
    std::unordered_map<std::string, TimeQuery::Ptr> time_query_cache;
    toolkit::Ticker cleanup_ticker;
    size_t roots_scanned = 0;
    size_t files_examined = 0;
    size_t candidates_observed = 0;
    size_t temporary_candidates = 0;
    size_t completed_orphan_candidates = 0;
    size_t timeline_groups_read = 0;
    size_t timeline_groups_skipped = 0;
    size_t stable_candidates = 0;
    size_t deleted_files = 0;
    uint64_t deleted_bytes = 0;
    size_t would_delete_files = 0;
    size_t errors = 0;
    bool budget_exhausted = false;
    size_t max_camera_count = 0;

    auto budgetExceeded = [&]() -> bool {
        if (files_examined >= static_cast<size_t>(max_files_per_cycle) ||
            cleanup_ticker.elapsedTime() >= static_cast<uint64_t>(work_budget_ms)) {
            budget_exhausted = true;
            return true;
        }
        return false;
    };
    auto baseName = [](const std::string &path) -> std::string {
        auto slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    };
    auto makeObservation = [](const struct stat &st) -> TemporaryMp4Observation {
        TemporaryMp4Observation observation;
        observation.device = static_cast<uint64_t>(st.st_dev);
        observation.inode = static_cast<uint64_t>(st.st_ino);
        observation.size = static_cast<int64_t>(st.st_size);
        observation.modified_at = static_cast<int64_t>(st.st_mtime);
        return observation;
    };
    auto getCleanupRetention = [&](const std::string &camera_id) -> CameraCleanupRetention {
        auto cached = retention_cache.find(camera_id);
        if (cached != retention_cache.end())
            return cached->second;

        CameraCleanupRetention result;
        auto effective = getEffectivePolicy(camera_id);
        auto policies = getPolicy(effective.policy_id);
        if (policies.empty() || policies.front().enabled == 0) {
            retention_cache[camera_id] = result;
            return result;
        }

        const std::string hot_tier = tierTypeToString(HotTier);
        for (const auto &tier : policies.front().parsedTiers()) {
            if (!tier.enabled || tier.tier != hot_tier)
                continue;
            if (tier.pool_id.empty() || tier.retain_until_days < 0)
                break;
            StoragePool policy_hot_pool;
            if (!getPoolById(tier.pool_id, policy_hot_pool) ||
                policy_hot_pool.enabled == 0 ||
                policy_hot_pool.tier != hot_tier ||
                poolTypeIsObjectStorage(policy_hot_pool.type)) {
                break;
            }

            uint64_t retention_seconds = static_cast<uint64_t>(tier.retain_until_days) * 86400ULL;
            auto recorder = StatisticRecorder::Instance().getRecorder(camera_id, false);
            if (recorder) {
                auto params = recorder->getParams();
                if (!params.option.keepArchivedMaxForAuto &&
                    params.option.keepArchivedMaxFor > 0) {
                    retention_seconds = std::min<uint64_t>(
                        retention_seconds, params.option.keepArchivedMaxFor);
                }
            }

            result.valid = true;
            if (retention_seconds < static_cast<uint64_t>(now)) {
                result.cutoff_date = StampUtils::getStartOfDay(static_cast<uint64_t>(now) - retention_seconds);
            }
            break;
        }
        retention_cache[camera_id] = result;
        return result;
    };

    for (size_t root_offset = 0; root_offset < roots.size(); ++root_offset) {
        if (budgetExceeded() || _stopping)
            break;

        const std::string &root = roots[(start_pool_index + root_offset) % roots.size()];
        ++roots_scanned;
        std::vector<std::string> camera_paths;
        File::scanDir(root, [&](const std::string &camera_path, bool) {
            struct stat camera_stat{};
            if (::lstat(camera_path.c_str(), &camera_stat) != 0) {
                if (errno != ENOENT)
                    ++errors;
                return true;
            }
            if (S_ISDIR(camera_stat.st_mode))
                camera_paths.push_back(camera_path);
            return true;
        }, false, false);
        std::sort(camera_paths.begin(), camera_paths.end());
        max_camera_count = std::max(max_camera_count, camera_paths.size());

        for (size_t camera_offset = 0; camera_offset < camera_paths.size(); ++camera_offset) {
            if (budgetExceeded() || _stopping)
                break;
            const std::string &camera_path = camera_paths[
                (start_camera_index + camera_offset) % camera_paths.size()];
            const std::string camera_id = baseName(camera_path);
            const CameraCleanupRetention retention = getCleanupRetention(camera_id);
            if (!retention.valid)
                continue;

            File::scanDir(camera_path, [&](const std::string &stream_path, bool) {
                if (budgetExceeded() || _stopping)
                    return false;
                struct stat stream_stat{};
                if (::lstat(stream_path.c_str(), &stream_stat) != 0) {
                    if (errno != ENOENT)
                        ++errors;
                    return true;
                }
                if (!S_ISDIR(stream_stat.st_mode))
                    return true;
                const std::string stream_id = baseName(stream_path);
                const std::string origin_stream = StrMediaUtils::originStream(stream_id);

                File::scanDir(stream_path, [&](const std::string &date_path, bool) {
                    if (budgetExceeded() || _stopping)
                        return false;
                    struct stat date_stat{};
                    if (::lstat(date_path.c_str(), &date_stat) != 0) {
                        if (errno != ENOENT)
                            ++errors;
                        return true;
                    }
                    if (!S_ISDIR(date_stat.st_mode))
                        return true;

                    const std::string date_name = baseName(date_path);
                    const uint64_t date_start = StrTimeUtils::getTsFromDateStr(date_name);
                    if (date_start == 0 ||
                        date_start >= retention.cutoff_date)
                        return true;

                    struct CompletedCandidate {
                        std::string path;
                        std::string segment_path;
                        uint64_t segment_time = 0;
                        TemporaryMp4Observation observation;
                    };
                    std::vector<CompletedCandidate> completed_candidates;

                    File::scanDir(date_path, [&](const std::string &path, bool) {
                        if (budgetExceeded() || _stopping)
                            return false;

                        struct stat file_stat{};
                        if (::lstat(path.c_str(), &file_stat) != 0) {
                            if (errno != ENOENT)
                                ++errors;
                            return true;
                        }
                        if (!S_ISREG(file_stat.st_mode))
                            return true;

                        ++files_examined;
                        const std::string name = baseName(path);
                        if (name.size() <= 4 || !end_with(name, ".mp4"))
                            return true;
                        if (!pathStartsWithRoot(path, root))
                            return true;

                        const bool is_temporary = name.front() == '.';
                        std::string segment_name = name.substr(0, name.size() - 4);
                        std::string segment_path = date_name + "/" + segment_name;
                        uint64_t segment_time = StrTimeUtils::getTsFromDateTimeStr(segment_path);
                        if (segment_time == 0)
                            segment_time = StrTimeUtils::getTsFromDateTimeStr2(segment_path);
                        if (segment_time == 0) {
                            return true;
                        }

                        if (!is_temporary) {
                            CompletedCandidate candidate;
                            candidate.path = path;
                            candidate.segment_path = segment_path;
                            candidate.segment_time = segment_time;
                            candidate.observation = makeObservation(file_stat);
                            completed_candidates.push_back(candidate);
                            return true;
                        }

                        if (!seen_candidates.insert(path).second)
                            return true;
                        ++candidates_observed;
                        ++temporary_candidates;
                        TemporaryMp4Observation observation = makeObservation(file_stat);
                        auto previous = previous_observations.find(path);
                        if (previous == previous_observations.end() || !(previous->second == observation)) {
                            current_observations[path] = observation;
                            return true;
                        }

                        ++stable_candidates;
                        struct stat verify_stat{};
                        if (::lstat(path.c_str(), &verify_stat) != 0) {
                            if (errno != ENOENT)
                                ++errors;
                            return true;
                        }
                        TemporaryMp4Observation verified = makeObservation(verify_stat);
                        if (!S_ISREG(verify_stat.st_mode) || !(verified == observation)) {
                            current_observations[path] = verified;
                            return true;
                        }

                        if (dry_run) {
                            ++would_delete_files;
                            current_observations[path] = verified;
                            return true;
                        }

                        if (::unlink(path.c_str()) == 0) {
                            ++deleted_files;
                            deleted_bytes += static_cast<uint64_t>(std::max<int64_t>(0, verified.size));
                        } else if (errno != ENOENT) {
                            ++errors;
                            current_observations[path] = verified;
                        }
                        return true;
                    }, false, true);

                    if (completed_candidates.empty())
                        return !budget_exhausted && !_stopping;

                    uint64_t query_start = completed_candidates.front().segment_time;
                    uint64_t query_end = query_start;
                    for (const auto &candidate : completed_candidates) {
                        query_start = std::min(query_start, candidate.segment_time);
                        query_end = std::max(query_end, candidate.segment_time);
                    }
                    if (query_end < std::numeric_limits<uint64_t>::max())
                        ++query_end;

                    std::vector<TimeBlock> blocks;
                    try {
                        const std::string query_key = camera_id + "\x1f" + stream_id;
                        TimeQuery::Ptr query;
                        auto query_it = time_query_cache.find(query_key);
                        if (query_it == time_query_cache.end()) {
                            MediaTuple tuple;
                            tuple.app = camera_id;
                            tuple.stream = stream_id;
                            query = std::make_shared<TimeQuery>(tuple);
                            time_query_cache.emplace(query_key, query);
                        } else {
                            query = query_it->second;
                        }
                        query->getRecordedTimePeriod(
                            query_start, query_end,
                            [&](std::vector<TimeBlock> &data) {
                                blocks = data;
                            });
                        ++timeline_groups_read;
                    } catch (const std::exception &ex) {
                        ++timeline_groups_skipped;
                        ++errors;
                        WarnL << "Skip completed MP4 cleanup, cannot query TimeFile: camera="
                              << camera_id << ", stream=" << stream_id
                              << ", date=" << date_name << ", error=" << ex.what();
                        return !budget_exhausted && !_stopping;
                    } catch (...) {
                        ++timeline_groups_skipped;
                        ++errors;
                        WarnL << "Skip completed MP4 cleanup, cannot query TimeFile: camera="
                              << camera_id << ", stream=" << stream_id
                              << ", date=" << date_name << ", unknown error";
                        return !budget_exhausted && !_stopping;
                    }

                    std::unordered_set<std::string> referenced_full_paths;
                    std::unordered_set<std::string> referenced_identities;
                    for (const auto &block : blocks) {
                        if (block.file_path().empty())
                            continue;
                        const std::string full_path = decodeBase64(block.file_path());
                        if (full_path.empty())
                            continue;
                        referenced_full_paths.insert(full_path);
                        std::string referenced_segment;
                        if (deriveSegmentPathFromFullPath(full_path, camera_id,
                                                          block.stream(), referenced_segment)) {
                            referenced_identities.insert(
                                StrMediaUtils::originStream(block.stream()) + "\x1f" +
                                referenced_segment);
                        }
                    }

                    for (const auto &candidate : completed_candidates) {
                        const std::string identity = origin_stream + "\x1f" + candidate.segment_path;
                        if (referenced_full_paths.count(candidate.path) != 0 ||
                            referenced_identities.count(identity) != 0) {
                            continue;
                        }

                        ++candidates_observed;
                        ++completed_orphan_candidates;
                        struct stat verify_stat{};
                        if (::lstat(candidate.path.c_str(), &verify_stat) != 0) {
                            if (errno != ENOENT)
                                ++errors;
                            continue;
                        }
                        const TemporaryMp4Observation verified = makeObservation(verify_stat);
                        if (!S_ISREG(verify_stat.st_mode) ||
                            !(verified == candidate.observation)) {
                            continue;
                        }

                        if (dry_run) {
                            ++would_delete_files;
                            continue;
                        }
                        if (::unlink(candidate.path.c_str()) == 0) {
                            ++deleted_files;
                            deleted_bytes += static_cast<uint64_t>(
                                std::max<int64_t>(0, verified.size));
                        } else if (errno != ENOENT) {
                            ++errors;
                        }
                    }
                    return !budget_exhausted && !_stopping;
                }, false, false);
                return !budget_exhausted && !_stopping;
            }, false, false);
        }
    }

    {
        std::lock_guard<std::mutex> lk(_temporary_mp4_observations_mtx);
        if (_stopping) {
            _temporary_mp4_observations.clear();
            _temporary_mp4_next_pool_index = 0;
            _temporary_mp4_next_camera_index = 0;
        } else {
            if (budget_exhausted) {
                for (const auto &entry : previous_observations) {
                    const std::string name = baseName(entry.first);
                    if (!name.empty() && name.front() == '.' &&
                        seen_candidates.count(entry.first) == 0) {
                        current_observations.emplace(entry.first, entry.second);
                    }
                }
            }
            _temporary_mp4_observations.swap(current_observations);
            _temporary_mp4_next_pool_index = (start_pool_index + 1) % roots.size();
            _temporary_mp4_next_camera_index = max_camera_count == 0
                ? 0 : (start_camera_index + 1) % max_camera_count;
        }
    }

    if (candidates_observed > 0 || deleted_files > 0 || would_delete_files > 0 || errors > 0) {
        DebugL << "cleanupOrphanTemporarySegments: roots_scanned=" << roots_scanned
               << ", files_examined=" << files_examined
               << ", candidates_observed=" << candidates_observed
               << ", temporary_candidates=" << temporary_candidates
               << ", completed_orphan_candidates=" << completed_orphan_candidates
               << ", timeline_groups_read=" << timeline_groups_read
               << ", timeline_groups_skipped=" << timeline_groups_skipped
               << ", stable_candidates=" << stable_candidates
               << ", deleted_files=" << deleted_files
               << ", deleted_bytes=" << deleted_bytes
               << ", would_delete_files=" << would_delete_files
               << ", errors=" << errors
               << ", budget_exhausted=" << budget_exhausted;
    }
}

void TierStorageManager::sweepStaleTimeBlocks() {
    GET_CONFIG(bool, enabled, Storage::kTierStaleBlockSweepEnabled);
    if (!enabled)
        return;
    GET_CONFIG(int64_t, work_budget_ms, Storage::kTierStaleBlockSweepWorkBudgetMs);
    GET_CONFIG(int, max_files, Storage::kTierStaleBlockSweepMaxFilesPerCycle);
    GET_CONFIG(int64_t, min_age_seconds, Storage::kTierReconcileMinAgeSeconds);
    if (work_budget_ms <= 0)
        work_budget_ms = 500;
    if (max_files <= 0)
        max_files = 20;

    GET_CONFIG(std::string, record_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    const std::string timeline_root = File::absolutePath(app_name, record_path);
    std::vector<std::string> cameras;
    File::scanDir(timeline_root, [&](const std::string &path, bool is_dir) {
        if (is_dir)
            cameras.push_back(path.substr(path.rfind('/') + 1));
        return true;
    }, false, false);
    if (cameras.empty())
        return;
    std::sort(cameras.begin(), cameras.end());

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const uint64_t today = StampUtils::getStartOfDay(static_cast<uint64_t>(now));
    // Only days before a camera's oldest live range are read: that is where retention leaves a
    // block it could not enumerate, and it keeps the pass from rereading every valid timefile of
    // the retention window. A camera with no live range at all has every past day eligible.
    std::unordered_map<std::string, uint64_t> oldest_live_day;
    for (const auto &oldest : SegmentTierRangeImp().findOldestStoredStartPerStream()) {
        const uint64_t day = StampUtils::getStartOfDay(static_cast<uint64_t>(std::max<int64_t>(0, oldest.start_time)));
        auto it = oldest_live_day.find(oldest.camera_id);
        if (it == oldest_live_day.end() || day < it->second)
            oldest_live_day[oldest.camera_id] = day;
    }
    // Live owners; DELETED/MISSING ranges no longer own their blocks.
    const std::string deleted = segmentStatusToString(SegmentStatus::DELETED);
    const std::string missing = segmentStatusToString(SegmentStatus::MISSING);
    std::unordered_map<std::string, bool> pool_readable;
    Ticker budget;
    int files_read = 0;
    size_t removed_blocks = 0;
    size_t start_index = _stale_block_next_camera_index % cameras.size();
    size_t visited = 0;

    for (; visited < cameras.size() && !_stopping; ++visited) {
        if (files_read >= max_files || budget.elapsedTime() >= static_cast<uint64_t>(work_budget_ms))
            break;
        const std::string &camera_id = cameras[(start_index + visited) % cameras.size()];
        const std::string camera_dir = timeline_root + "/" + camera_id;
        auto oldest_it = oldest_live_day.find(camera_id);
        const uint64_t before_day = oldest_it == oldest_live_day.end()
            ? today : std::min<uint64_t>(today, oldest_it->second);

        std::map<uint64_t, std::string> past_files;
        File::scanDir(camera_dir, [&](const std::string &path, bool is_dir) {
            if (is_dir || !end_with(path, ".s3db"))
                return true;
            const std::string name = path.substr(path.rfind('/') + 1);
            // ".<date>.s3db" is a rebuild in progress.
            if (name.empty() || name[0] == '.')
                return true;
            const uint64_t day = StrTimeUtils::getTsFromDateStr(name.substr(0, name.size() - 5));
            if (day == 0 || day >= before_day)
                return true;
            // Recently written (gap fill, SD-card import): its ranges may not be registered yet.
            struct stat st{};
            if (::stat(path.c_str(), &st) != 0 || st.st_mtime > now - min_age_seconds)
                return true;
            past_files.emplace(day, path);
            return true;
        }, false, false);

        std::vector<TimeBlockIdentity> stale;
        std::string first_stale;
        for (const auto &file : past_files) {
            if (files_read >= max_files || budget.elapsedTime() >= static_cast<uint64_t>(work_budget_ms))
                break;
            ++files_read;
            const int64_t day_start = static_cast<int64_t>(file.first);
            std::vector<TimeBlock> blocks;
            try {
                // A full scan: the owner check must not depend on block order or the index.
                TimeDemuxer demuxer;
                demuxer.openFile(file.second);
                bool eof = false;
                while (!eof) {
                    TimeBlock block;
                    demuxer.readBlock(block, eof);
                    if (!eof)
                        blocks.push_back(std::move(block));
                }
            } catch (const std::exception &ex) {
                WarnL << "Skip stale block sweep of " << file.second << ": " << ex.what();
                continue;
            }
            if (blocks.empty())
                continue;

            std::unordered_map<std::string, std::vector<SegmentTierRange>> owners;
            for (auto &range : SegmentTierRangeImp().queryByCamera(camera_id, day_start - 86400,
                                                                   day_start + 2 * 86400)) {
                if (range.status != deleted && range.status != missing)
                    owners[range.stream_id].push_back(std::move(range));
            }

            for (const auto &block : blocks) {
                const int64_t block_start = static_cast<int64_t>(block.start_time());
                bool owned = false;
                for (const auto &range : owners[block.stream()]) {
                    if (range.start_time <= block_start && block_start < range.end_time) {
                        owned = true;
                        break;
                    }
                }
                if (owned || block.file_path().empty())
                    continue;
                const std::string full_path = decodeBase64(block.file_path());
                std::string segment_path;
                if (!deriveSegmentPathFromFullPath(full_path, camera_id, block.stream(), segment_path))
                    continue;
                StoragePool pool;
                std::string pool_root;
                if (!resolveHotPoolForPath(full_path, pool, pool_root))
                    continue;
                auto readable = pool_readable.find(pool.id);
                if (readable == pool_readable.end())
                    readable = pool_readable.emplace(pool.id, isFilePoolVerifiablyReadable(pool)).first;
                if (!readable->second)
                    continue;
                // Gone only when neither spelling exists; any other stat error proves nothing.
                const std::string plain = RecordEncryptionUtils::stripEncSuffix(full_path);
                bool gone = true;
                for (const auto &path : {plain, plain + RecordEncryptionUtils::encryptedFileSuffix()}) {
                    struct stat st{};
                    if (::stat(path.c_str(), &st) == 0 || errno != ENOENT) {
                        gone = false;
                        break;
                    }
                }
                if (!gone)
                    continue;
                TimeBlockIdentity identity;
                identity.camera_id = camera_id;
                identity.stream_id = block.stream();
                identity.start_time = block.start_time();
                identity.segment_path = segment_path;
                stale.push_back(identity);
                if (first_stale.empty())
                    first_stale = full_path;
            }
        }

        if (stale.empty())
            continue;
        // Rewrites only the days holding these blocks; a day left empty loses its timefile.
        StorageManager::Instance().removeCameraTimeBlocks(camera_id, stale);
        removed_blocks += stale.size();
        WarnL << "Removed " << stale.size() << " stale timefile block(s) without range or MP4, camera="
              << camera_id << ", first=" << first_stale;
        StorageAlert alert;
        alert.id = StrUUID::make_guid(8, "alert");
        alert.level = "WARNING";
        alert.type = "TIMEFILE_STALE_BLOCK_REMOVED";
        alert.message = StrPrinter << "Removed " << stale.size()
                                   << " timeline block(s) with no live range and no MP4, camera="
                                   << camera_id << ", first=" << first_stale;
        alert.created_at = now;
        StorageAlertImp().add(alert);
    }
    _stale_block_next_camera_index = (start_index + std::max<size_t>(1, visited)) % cameras.size();
    if (removed_blocks > 0 || files_read > 0) {
        DebugL << "sweepStaleTimeBlocks: cameras=" << visited << ", files_read=" << files_read
               << ", removed_blocks=" << removed_blocks;
    }
}

// ===========================================================================
// Record encryption support
// ===========================================================================

std::string TierStorageManager::resolveRecordFilePath(const std::string &path) const {
    return resolveLocalVariant(path);
}

void TierStorageManager::setEncryptionActive(bool active) {
    s_encryption_active.store(active, std::memory_order_relaxed);
}

void TierStorageManager::acquireSegmentLock(const std::string &path) {
    const std::string key = RecordEncryptionUtils::stripEncSuffix(path);
    std::unique_lock<std::mutex> lock(_segment_busy_mtx);
    _segment_busy_cv.wait(lock, [&] { return _segment_busy.count(key) == 0; });
    _segment_busy.insert(key);
}

void TierStorageManager::releaseSegmentLock(const std::string &path) {
    const std::string key = RecordEncryptionUtils::stripEncSuffix(path);
    {
        std::lock_guard<std::mutex> lock(_segment_busy_mtx);
        _segment_busy.erase(key);
    }
    _segment_busy_cv.notify_all();
}

TierStorageManager::SegmentFileLock::SegmentFileLock(TierStorageManager *owner, const std::string &path)
    : _owner(owner), _key(RecordEncryptionUtils::stripEncSuffix(path)) {
    _owner->acquireSegmentLock(_key);
}

TierStorageManager::SegmentFileLock::~SegmentFileLock() {
    _owner->releaseSegmentLock(_key);
}

bool TierStorageManager::isSegmentFileBusy(const std::string &path) const {
    std::lock_guard<std::mutex> lock(_segment_busy_mtx);
    return _segment_busy.count(RecordEncryptionUtils::stripEncSuffix(path)) != 0;
}

bool TierStorageManager::deriveSegmentPathForFile(const std::string &camera_id,
                                                  const std::string &stream_id,
                                                  const std::string &full_path,
                                                  std::string &out_segment_path) {
    return deriveSegmentPathFromFullPath(full_path, camera_id, stream_id, out_segment_path);
}

static std::atomic<bool> s_origin_pool_column_missing{false};
static void reportOriginPoolQueryFailure(const char *where, const char *what) {
    if (!s_origin_pool_column_missing.exchange(true)) {
        WarnL << where << ": query on segment_tier_ranges.origin_pool_id failed ("
              << (what ? what : "unknown error")
              << "). The column is most likely missing -- apply the record encryption migration. "
                 "Record encryption cannot resolve the originating pool until then; tier storage "
                 "itself is unaffected.";
    }
}

bool TierStorageManager::findRangeForSegment(const std::string &camera_id,
                                             const std::string &stream_id,
                                             int64_t segment_start_time,
                                             SegmentTierRange &out_range) {
    if (camera_id.empty() || stream_id.empty() || segment_start_time <= 0)
        return false;

    SegmentTierRangeImp range_imp;
    auto ranges = range_imp.queryByCamera(camera_id, segment_start_time, segment_start_time + 1);
    for (const auto &r : ranges) {
        if (r.stream_id != stream_id)
            continue;
        if (r.start_time <= segment_start_time && r.end_time > segment_start_time &&
            r.status != segmentStatusToString(SegmentStatus::DELETED) &&
            r.status != segmentStatusToString(SegmentStatus::EXPIRED) &&
            r.status != segmentStatusToString(SegmentStatus::MISSING)) {
            out_range = r;
            return true;
        }
    }
    return false;
}

std::string TierStorageManager::getSegmentOriginPoolId(const std::string &camera_id,
                                                       const std::string &stream_id,
                                                       int64_t segment_start_time) {
    SegmentTierRange range;
    if (!findRangeForSegment(camera_id, stream_id, segment_start_time, range))
        return "";

    if (!range.origin_pool_id.empty())
        return range.origin_pool_id;
    // Still in HOT, so the pool it lives in is the pool that wrote it.
    if (range.tier == tierTypeToString(HotTier))
        return range.pool_id;

    WarnL << "Segment range is missing origin_pool_id and has already left the HOT tier; "
          << "cannot determine the encryption key: range=" << range.range_id
          << " camera=" << camera_id << " start=" << segment_start_time;
    return "";
}

std::vector<TierRangeSegmentFile> TierStorageManager::listSegmentFilesByOriginPool(const std::string &origin_pool_id,
                                                                                   int64_t start_time,
                                                                                   int64_t end_time) {
    std::vector<TierRangeSegmentFile> ret;
    if (origin_pool_id.empty())
        return ret;
    if (s_origin_pool_column_missing.load(std::memory_order_relaxed))
        return ret;

    std::vector<SegmentTierRange> ranges;
    try {
        SegmentTierRangeImp range_imp;
        ranges = range_imp.queryByOriginPool(origin_pool_id, start_time, end_time);
    } catch (const std::exception &ex) {
        reportOriginPoolQueryFailure("listSegmentFilesByOriginPool", ex.what());
        return ret;
    } catch (...) {
        reportOriginPoolQueryFailure("listSegmentFilesByOriginPool", nullptr);
        return ret;
    }

    for (const auto &range : ranges) {
        auto files = collectSegmentFilesForRange(range);
        for (auto &f : files) {
            if (f.origin_pool_id.empty())
                f.origin_pool_id = origin_pool_id;
            ret.emplace_back(std::move(f));
        }
    }
    return ret;
}

int TierStorageManager::countSegmentsByOriginPool(const std::string &origin_pool_id) {
    if (origin_pool_id.empty())
        return 0;

    std::vector<SegmentTierRange> ranges;
    try {
        SegmentTierRangeImp range_imp;
        ranges = range_imp.queryByOriginPool(origin_pool_id, 0, 0);
    } catch (const std::exception &ex) {
        reportOriginPoolQueryFailure("countSegmentsByOriginPool", ex.what());
        // -1 means "cannot tell", not "nothing depends on this pool". Callers that
        // gate a destructive action on this must refuse rather than assume zero.
        return -1;
    } catch (...) {
        reportOriginPoolQueryFailure("countSegmentsByOriginPool", nullptr);
        return -1;
    }

    int count = 0;
    for (const auto &r : ranges) {
        if (r.status == segmentStatusToString(SegmentStatus::DELETED) ||
            r.status == segmentStatusToString(SegmentStatus::EXPIRED))
            continue;
        count += static_cast<int>(std::max<int64_t>(0, r.segment_count));
    }
    return count;
}

bool TierStorageManager::replaceSegmentInPlace(const std::string &camera_id,
                                                const std::string &stream_id,
                                                const std::string &segment_path,
                                                const std::string &new_local_file,
                                                std::string *err) {
    auto fail = [&](const std::string &msg) {
        if (err) *err = msg;
        WarnL << "replaceSegmentInPlace: " << msg;
        return false;
    };

    if (camera_id.empty() || stream_id.empty() || segment_path.empty() || new_local_file.empty())
        return fail("Missing arguments");
    if (!regularFileExists(new_local_file))
        return fail("Replacement file does not exist: " + new_local_file);

    int64_t segment_start_time = RecordEncryptionUtils::parseRecordingTimestamp(new_local_file);
    SegmentTierRange range;
    if (segment_start_time <= 0 || !findRangeForSegment(camera_id, stream_id, segment_start_time, range))
        return fail("No segment range covers " + new_local_file);

    StoragePool pool;
    if (!getPoolById(range.pool_id, pool))
        return fail("Pool not found: " + range.pool_id);

    // The key must describe the file we are actually writing. Building it from
    // segment_path alone meant an encrypted file was written back under the
    // plaintext key, leaving the ".enc" file orphaned and the pool holding two
    // copies of every segment.
    const std::string base_key = TierFileStorageBase::makeStorageKey(camera_id, stream_id, segment_path);
    const std::string storage_key = keyForVariant(base_key, new_local_file);
    const std::string stale_key = RecordEncryptionUtils::toggleEncSuffix(storage_key);

    if (poolTypeIsObjectStorage(pool.type)) {
        if (!_obj_storage.isRegistered(pool.id) && !registerPoolObjStorage(pool))
            return fail("Object storage pool is not registered: " + pool.id);

        const std::string base_s3_key = TierObjectStorage::makeS3Key(pool.base_path.value_or(""), camera_id, stream_id, segment_path);
        const std::string s3_key = keyForVariant(base_s3_key, new_local_file);
        if (!_obj_storage.uploadSegment(pool.id, new_local_file, s3_key))
            return fail("Failed to upload to object storage, key=" + s3_key);

        // Drop the object stored under the previous spelling so the bucket does
        // not keep both a plaintext and an encrypted copy.
        const std::string stale_s3_key = RecordEncryptionUtils::toggleEncSuffix(s3_key);
        if (stale_s3_key != s3_key)
            _obj_storage.deleteSegment(pool.id, stale_s3_key);

        for (const auto &job : RestoreJobImp().queryCompletedForSegment(camera_id, stream_id, segment_path)) {
            const std::string plain = buildRestoreSegmentPath(job.job_id, camera_id, stream_id, segment_path);
            if (plain.empty())
                continue;
            for (const std::string &cached : { plain, plain + RecordEncryptionUtils::encryptedFileSuffix() }) {
                if (::unlink(cached.c_str()) == 0)
                    DebugL << "Deleted old restore cache after replacing segment: " << cached;
            }
        }
        const std::string legacy = buildLegacyRestoreSegmentPath(camera_id, stream_id, segment_path);
        for (const std::string &cached : { legacy, legacy + RecordEncryptionUtils::encryptedFileSuffix() }) {
            if (::unlink(cached.c_str()) == 0)
                DebugL << "Deleted old restore cache after replacing segment: " << cached;
        }
    } else {
        auto storage = fileStorageForPoolType(pool.type);
        if (!storage)
            return fail("File storage type is not supported: " + pool.type);
        if (!storage->isRegistered(pool.id) && !registerPoolFileStorage(pool))
            return fail("File storage pool is not registered: " + pool.id);

        std::string dst_path = storage->resolvePath(pool.id, storage_key);
        if (dst_path.empty())
            return fail("Failed to resolve destination path in pool: " + pool.id);

        if (isSameFileOnDisk(new_local_file, dst_path)) {
            DebugL << "Segment is already at the destination path, skipping copy: " << dst_path;
        } else if (!storage->uploadSegment(pool.id, new_local_file, storage_key)) {
            return fail("Failed to overwrite segment: " + dst_path);
        }

        if (stale_key != storage_key) {
            std::string stale_path = storage->resolvePath(pool.id, stale_key);
            if (!stale_path.empty() && !isSameFileOnDisk(stale_path, dst_path) &&
                ::unlink(stale_path.c_str()) == 0) {
                DebugL << "Removed stale segment variant: " << stale_path;
            }
        }
    }

    // An encrypted file carries an extra trailer, so the range's accounting is
    // now stale by that much.
    SegmentTierRangeImp range_imp;
    range_imp.touchUpdatedAt(range.range_id);
    return true;
}

void TierStorageManager::registerEncryptionStorageBridge() {
    EncryptionStorageBridge bridge;

    bridge.getPoolInfo = [](const std::string &pool_id) {
        BridgePoolInfo info;
        auto pools = TierStorageManager::Instance().getPool(pool_id);
        if (pools.empty()) return info;
        const auto &pool = pools.front();
        info.found = true;
        info.tier = pool.tier;
        info.mount_path = pool.mount_path.value_or("");
        info.network_path = pool.network_path.value_or("");
        return info;
    };

    bridge.lockSegmentFile = [](const std::string &path) {
        TierStorageManager::Instance().acquireSegmentLock(path);
    };

    bridge.unlockSegmentFile = [](const std::string &path) {
        TierStorageManager::Instance().releaseSegmentLock(path);
    };

    bridge.deriveSegmentPathForFile = [](const std::string &camera_id, const std::string &stream_id,
                                          const std::string &full_path, std::string &out_segment_path) {
        return TierStorageManager::Instance().deriveSegmentPathForFile(camera_id, stream_id, full_path, out_segment_path);
    };

    bridge.replaceSegmentInPlace = [](const std::string &camera_id, const std::string &stream_id,
                                       const std::string &segment_path, const std::string &new_local_file,
                                       std::string *err) {
        return TierStorageManager::Instance().replaceSegmentInPlace(camera_id, stream_id, segment_path, new_local_file, err);
    };

    bridge.listSegmentFilesByOriginPool = [](const std::string &origin_pool_id, int64_t start_time, int64_t end_time) {
        std::vector<BridgeSegmentFile> out;
        for (const auto &sf : TierStorageManager::Instance().listSegmentFilesByOriginPool(origin_pool_id, start_time, end_time)) {
            BridgeSegmentFile item;
            item.camera_id    = sf.camera_id;
            item.stream_id    = sf.stream_id;
            item.segment_path = sf.segment_path;
            item.full_path    = sf.full_path;
            item.start_time   = sf.start_time;
            item.end_time     = sf.end_time;
            item.file_size    = sf.file_size;
            item.encrypted    = sf.encrypted;
            item.tier         = sf.tier;
            item.pool_id      = sf.pool_id;
            out.push_back(std::move(item));
        }
        return out;
    };

    // Deliberately the raw resolution: the only caller is the re-encryption job, which has to
    // see the file as it is on disk. Handing it the decrypting variant gave it the plaintext
    // copy from the decrypt cache, so every segment looked unencrypted and the job skipped it.
    bridge.resolvePlaybackSegmentPath = [](const std::string &camera_id, const std::string &stream_id,
                                            int64_t segment_start_time, const std::string &timefile_path) {
        BridgePlaybackPathResult out;
        auto r = TierStorageManager::Instance().resolvePlaybackSegmentPathRaw(camera_id, stream_id, segment_start_time, timefile_path);
        out.ready             = r.ready;
        out.restore_required  = r.restore_required;
        out.read_path         = r.read_path;
        out.message           = r.message;
        return out;
    };

    bridge.requestRestoreSegment = [](const std::string &camera_id, const std::string &stream_id,
                                       int64_t segment_start_time, const std::string &path_hint,
                                       const std::string &reason) {
        BridgeRestoreResult out;
        auto r = TierStorageManager::Instance().requestRestoreSegment(camera_id, stream_id, segment_start_time, path_hint, reason);
        out.handled = r.handled;
        out.job_id  = r.job_id;
        out.message = r.message;
        return out;
    };

    bridge.getRestoreJobStatus = [](const std::string &job_id, std::string &out_status) {
        auto jobs = TierStorageManager::Instance().getRestoreJob(job_id);
        if (jobs.empty()) return false;
        out_status = jobs.front().status;
        return true;
    };

    bridge.resolveRecordFilePath = [](const std::string &path) {
        return TierStorageManager::Instance().resolveRecordFilePath(path);
    };

    bridge.getSegmentOriginPoolId = [](const std::string &camera_id, const std::string &stream_id, int64_t segment_start_time) {
        return TierStorageManager::Instance().getSegmentOriginPoolId(camera_id, stream_id, segment_start_time);
    };

    bridge.isSegmentFileBusy = [](const std::string &path) {
        return TierStorageManager::Instance().isSegmentFileBusy(path);
    };

    bridge.setEncryptionActive = [](bool active) {
        TierStorageManager::Instance().setEncryptionActive(active);
    };

    TierEncryptionManager::Instance().bindStorageBridge(std::move(bridge));
}

static void* s_tag;

static onceToken g_token(
[]() {
#ifdef ENABLE_MP4
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        int64_t start_time = static_cast<int64_t>(info.start_time);
        int64_t end_time = start_time + std::max<int64_t>(1, static_cast<int64_t>(std::round(info.time_len)));
        auto origin_stream = StrMediaUtils::originStream(info.stream);
        // An exception escaping a listener stops NoticeCenter from calling the listeners after it
        // (encryption, motion .mblk) for this segment, so a busy database must stop here.
        try {
            TierStorageManager::Instance().registerHotSegmentRange(info.app, origin_stream, info.file_path, start_time, end_time, static_cast<int64_t>(info.file_size));
        } catch (const std::exception &ex) {
            WarnL << "Register HOT segment range failed camera=" << info.app << " stream=" << origin_stream
                  << " path=" << info.file_path << ": " << ex.what();
        }
    });
#endif
},
[]() {
#ifdef ENABLE_MP4
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastRecordMP4);
#endif
});

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
