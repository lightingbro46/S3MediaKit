#include <ctime>
#include <cstring>
#include <cmath>
#include <cctype>
#include <limits>
#include <unordered_set>
#include <sys/stat.h>
#include <unistd.h>

#include "Util/base64.h"
#include "Util/util.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Common/StrUtil.h"
#include "Thread/WorkThreadPool.h"
#include "Server/GlobalMonitor.h"
#include "Local/StatisticRecorder.h"
#include "Local/TimeQuery.h"
#include "Local/TimeRebuilder.h"
#include "Local/MotionBlockManager.h"
#include "Common/DeviceSource.h"
#include "Storage/Bookmark.h"
#include "Storage/TierBackfillTask.h"

#include "TierStorageManager.h"
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

static const char kDefaultHotPoolId[] = "pool-default-hot";
static const char kDefaultHotPoolName[] = "Default HOT Tier Storage";
static const char kSystemDefaultPolicyId[] = "policy-system-default";
static const char kSystemDefaultPolicyName[] = "System Default Policy";

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
        if (isDir || !end_with(path, ".mp4"))
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
    if (!end_with(segment_path, ".mp4"))
        return false;
    segment_path.resize(segment_path.size() - 4);
    return !segment_path.empty();
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
        return buildRestoreSegmentPath(job.job_id, segment.camera_id,
                                       segment.stream_id, segment.segment_path);
    return buildLegacyRestoreSegmentPath(segment.camera_id, segment.stream_id,
                                         segment.segment_path);
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

    return _obj_storage.registerPool(
        pool.id, pool.type,
        pool.endpoint.value(),
        pool.bucket.value(),
        base,
        pool.access_key.value(),
        secret);
}

void TierStorageManager::unregisterPoolObjStorage(const std::string &pool_id) {
    _obj_storage.unregisterPool(pool_id);
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

    // Jobs left RUNNING by a crash are safe to claim again from durable state.
    TieringJobImp().recoverRunning();
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
        std::unique_lock<std::mutex> lk(_health_cycle_state->mutex);
        while (_health_cycle_state->running)
            _health_cycle_state->completed.wait(lk);
    }
    {
        std::unique_lock<std::mutex> lk(_transfer_dispatch_state->mutex);
        while (_transfer_dispatch_state->active_total > 0)
            _transfer_dispatch_state->completed.wait(lk);
    }
    _backfill_tasks_initialized = false;
    {
        std::lock_guard<std::mutex> lk(_temporary_mp4_observations_mtx);
        _temporary_mp4_observations.clear();
        _temporary_mp4_next_pool_index = 0;
    }
    InfoL << "TierStorageManager stopped";
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
    try {
        TimeQuery query(tuple, task.timeline_path, false);
        query.getRecordedTimePeriod(
            static_cast<uint64_t>(chunk.start_time),
            static_cast<uint64_t>(chunk.end_time),
            [&](std::vector<TimeBlock> &data) {
                blocks = data;
            });
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    } catch (...) {
        error = "Unknown timeline open error";
        return false;
    }

    const std::string hot_tier = tierTypeToString(HotTier);
    const std::string available = segmentStatusToString(SegmentStatus::AVAILABLE);
    std::unordered_set<std::string> seen_segments;
    std::unordered_map<std::string, size_t> bucket_indexes;
    std::unordered_map<std::string, std::vector<SegmentTierRange>> existing_by_stream;
    for (const auto &range : SegmentTierRangeImp().queryByCamera(
             task.camera_id, chunk.start_time, chunk.end_time)) {
        existing_by_stream[range.stream_id].push_back(range);
    }
    for (const auto &block : blocks) {
        ++chunk.scanned_blocks;
        int64_t block_start = static_cast<int64_t>(block.start_time());
        int64_t block_end = block_start + std::max<int64_t>(
            1, static_cast<int64_t>(block.time_len()));
        bool already_indexed = false;
        auto existing_it = existing_by_stream.find(block.stream());
        if (existing_it != existing_by_stream.end()) {
            for (const auto &range : existing_it->second) {
                if (range.start_time <= block_start && range.end_time >= block_end) {
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
            if (error.empty())
                error = "Timeline block has no usable stream or file path";
            continue;
        }

        StoragePool hot_pool;
        std::string hot_root;
        if (!resolveHotPoolForPath(full_path, hot_pool, hot_root)) {
            ++chunk.orphan_blocks;
            if (error.empty())
                error = "No HOT pool matches timeline path: " + full_path;
            continue;
        }

        std::string identity = block.stream() + "\x1f" + full_path;
        if (!seen_segments.insert(identity).second)
            continue;

        uint64_t raw_size = block.file_size();
        int64_t file_size = raw_size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::max()
            : static_cast<int64_t>(raw_size);
        int64_t now = static_cast<int64_t>(time(nullptr));

        const int64_t bucket_start = static_cast<int64_t>(
            StampUtils::getStartOfDay(static_cast<uint64_t>(block_start)));
        const std::string bucket_key = block.stream() + "\x1f" + hot_pool.id +
            "\x1f" + std::to_string(bucket_start);
        auto bucket_it = bucket_indexes.find(bucket_key);
        if (bucket_it == bucket_indexes.end()) {
            SegmentTierRange range;
            range.range_id = StrUUID::make_guid(8, "rng");
            range.camera_id = task.camera_id;
            range.stream_id = block.stream();
            range.tier = hot_tier;
            range.pool_id = hot_pool.id;
            range.status = available;
            range.start_time = block_start;
            range.end_time = block_end;
            range.segment_count = 1;
            range.size_bytes = std::max<int64_t>(0, file_size);
            range.created_at = now;
            range.updated_at = now;
            bucket_indexes[bucket_key] = chunk.ranges.size();
            chunk.ranges.push_back(range);
        } else {
            auto &range = chunk.ranges[bucket_it->second];
            range.start_time = std::min(range.start_time, block_start);
            range.end_time = std::max(range.end_time, block_end);
            ++range.segment_count;
            range.size_bytes += std::max<int64_t>(0, file_size);
        }
        ++chunk.registered_blocks;
        chunk.indexed_bytes += std::max<int64_t>(0, file_size);
    }
    return chunk.orphan_blocks == 0;
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

void TierStorageManager::runBackfillCycle() {
#ifdef ENABLE_MP4
    GET_CONFIG(bool, backfill_enabled, Storage::kTierRangeBackfillEnabled);
    if (!backfill_enabled || _stopping || _backfill_running.exchange(true))
        return;

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self]() {
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

std::vector<TierRangeSegmentFile> TierStorageManager::collectSegmentFilesForRange(const SegmentTierRange &range) {
    std::vector<TierRangeSegmentFile> ret;
    if (range.camera_id.empty() || range.stream_id.empty() ||
        range.start_time <= 0 || range.end_time <= 0) {
        return ret;
    }

    try {
        mediakit::MediaTuple tuple;
        tuple.app = range.camera_id;
        tuple.stream = range.stream_id;
        TimeQuery query(tuple, "", false);
        uint64_t start = static_cast<uint64_t>(range.start_time);
        uint64_t end = static_cast<uint64_t>(range.end_time);
        query.getRecordedTimePeriod(start, end, [&](std::vector<TimeBlock> &blocks) {
            for (const auto &block : blocks) {
                if (block.app() != range.camera_id || block.stream() != range.stream_id)
                    continue;

                int64_t block_start = static_cast<int64_t>(block.start_time());
                int64_t block_end = block_start + std::max<int64_t>(1, static_cast<int64_t>(block.time_len()));
                if (block_end <= range.start_time || block_start >= range.end_time)
                    continue;

                std::string timefile_path;
                if (!block.file_path().empty())
                    timefile_path = decodeBase64(block.file_path());

                std::string segment_path;
                if (!deriveSegmentPathFromFullPath(timefile_path, range.camera_id, range.stream_id, segment_path))
                    continue;

                TierRangeSegmentFile item;
                item.camera_id = range.camera_id;
                item.stream_id = range.stream_id;
                item.segment_path = segment_path;
                item.storage_key = TierFileStorageBase::makeStorageKey(item.camera_id, item.stream_id, item.segment_path);
                item.start_time = block_start;
                item.end_time = block_end;
                item.file_size = static_cast<int64_t>(block.file_size());

                if (!range.pool_id.empty()) {
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
                    if (has_pool && !poolTypeIsObjectStorage(pool.type)) {
                        auto storage = fileStorageForPoolType(pool.type);
                        if (storage && (storage->isRegistered(pool.id) || registerPoolFileStorage(pool)))
                            item.full_path = storage->resolvePath(pool.id, item.storage_key);
                    }
                }
                if (item.full_path.empty())
                    item.full_path = timefile_path;

                ret.emplace_back(std::move(item));
            }
        });
    } catch (const std::exception &ex) {
        WarnL << "collectSegmentFilesForRange failed range=" << range.range_id
              << " camera=" << range.camera_id << ": " << ex.what();
    } catch (...) {
        WarnL << "collectSegmentFilesForRange failed range=" << range.range_id
              << " camera=" << range.camera_id;
    }
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
    PlaybackPathResolveResult ret;
    ret.read_path = timefile_path;
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
    std::string resolved_path = storage->resolvePath(pool.id, storage_key);
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
        bool retain_restore_cache) {
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

std::vector<TierRangeSegmentFile> TierStorageManager::collectSegmentFilesForJob(const TieringJob &job,
                                                                               SegmentTierRange &out_range) {
    SegmentTierRangeImp range_imp;
    if (!job.range_id.empty()) {
        auto ranges = range_imp.findByRangeIds({job.range_id});
        if (!ranges.empty()) {
            out_range = ranges.front();
            if (out_range.camera_id != job.camera_id ||
                out_range.tier != job.source_tier ||
                out_range.pool_id != job.source_pool_id ||
                out_range.status != segmentStatusToString(SegmentStatus::AVAILABLE)) {
                return {};
            }
            return collectSegmentFilesForRange(out_range);
        }
        return {};
    }

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
        return collectSegmentFilesForRange(out_range);
    }
    return {};
}

bool TierStorageManager::splitRangeForTieringThreshold(const SegmentTierRange &range,
                                                       int64_t move_threshold,
                                                       SegmentTierRange &out_move_range) {
    out_move_range = SegmentTierRange();
    if (range.range_id.empty() || range.pool_id.empty())
        return false;
    if (range.start_time >= move_threshold)
        return false;
    if (range.end_time <= move_threshold) {
        out_move_range = range;
        return true;
    }

    auto segments = collectSegmentFilesForRange(range);
    if (segments.empty()) {
        WarnL << "Cannot split tier range without timefile segments, range=" << range.range_id
              << " camera=" << range.camera_id;
        return false;
    }

    std::sort(segments.begin(), segments.end(), [](const TierRangeSegmentFile &a,
                                                   const TierRangeSegmentFile &b) {
        return a.start_time < b.start_time;
    });

    int64_t split_time = 0;
    int64_t move_count = 0;
    int64_t move_size = 0;
    int64_t remain_count = 0;
    int64_t remain_size = 0;
    for (const auto &seg : segments) {
        if (seg.end_time <= move_threshold) {
            split_time = std::max(split_time, seg.end_time);
            ++move_count;
            move_size += std::max<int64_t>(0, seg.file_size);
        } else {
            ++remain_count;
            remain_size += std::max<int64_t>(0, seg.file_size);
        }
    }
    if (split_time <= range.start_time)
        return false;
    if (split_time >= range.end_time) {
        out_move_range = range;
        return true;
    }

    int64_t now = static_cast<int64_t>(time(nullptr));
    SegmentTierRange move_piece = range;
    SegmentTierRange remain_piece = range;
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

    SegmentTierRangeImp range_imp;
    if (!range_imp.replaceRangeWithPieces(range.range_id, {move_piece, remain_piece})) {
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
    WorkThreadPool::Instance().getPoller()->async([weak_self]() {
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

        self->ensureSystemDefaultPolicy();

        // Resume durable work without blocking the maintenance scan.
        self->dispatchPendingTieringJobs();

        // Gather cameras from compact ranges.
        SegmentTierRangeImp range_imp;
        auto camera_ids = range_imp.findDistinctAvailableCameras();

        for (const auto &cam_id : camera_ids) {
            try {
                auto eff = self->getEffectivePolicy(cam_id);
                if (!eff.policy_id.empty()) {
                    auto policies = self->getPolicy(eff.policy_id);
                    if (!policies.empty()) {
                        const auto &policy = policies.front();
                        self->processCameraTiering(cam_id, policy);
                        self->processCameraPressureTiering(cam_id, policy);
                        self->enforcePolicyDeleteRetention(cam_id, policy);
                        self->enforceCameraArchiveRetention(cam_id, policy);
                    }
                }
            } catch (const std::exception &e) {
                WarnL << "Error processing camera " << cam_id << ": " << e.what();
            }
        }

            self->processMetadataCleanupJobs();
            self->processRestoreJobs();
            self->pruneOldMetrics();
            self->cleanupRestoreTempFiles();
            self->cleanupOrphanTemporarySegments();
            self->dispatchPendingTieringJobs();
            InfoL << "Tiering cycle done. Elapsed: " << format_duration_verbose(self->_ticker.elapsedTime());
        } catch (const std::exception &ex) {
            WarnL << "Tier storage maintenance cycle failed: " << ex.what();
        } catch (...) {
            WarnL << "Tier storage maintenance cycle failed with unknown error";
        }
    });
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
        DebugL << "Processing tiering for camera=" << camera_id
               << " source_tier=" << src_tier_cfg.tier
               << " target_tier=" << (dst_tier_cfg ? dst_tier_cfg->tier : "")
               << " move_threshold=" << getTimeStr("%Y-%m-%d %H:%M:%S", move_threshold);
        auto ranges = range_imp.findByCameraTierStartedBefore(
            camera_id, src_tier_cfg.tier, move_threshold, max_ranges_per_cycle);

        for (const auto &range : ranges) {
            if (range.pool_id.empty()) continue;
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

bool TierStorageManager::queueTierMoveJob(const SegmentTierRange &range,
                                          const PolicyTierConfig &src_tier_cfg,
                                          const PolicyTierConfig &dst_tier_cfg,
                                          const StoragePolicy &policy,
                                          TieringJobTrigger trigger) {
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

    TieringJobImp job_imp;
    auto existing_jobs = job_imp.query(range.camera_id, "", src_tier_cfg.tier, dst_tier_cfg.tier);
    for (const auto &j : existing_jobs) {
        if (j.segment_start_time == range.start_time &&
            j.segment_end_time == range.end_time &&
            (j.status == jobStatusToString(JobStatus::PENDING) ||
             j.status == jobStatusToString(JobStatus::RUNNING))) {
            return false;
        }
    }

    int64_t now = static_cast<int64_t>(time(nullptr));
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

    if (!job_imp.add(job))
        return false;

    DebugL << "Queued " << (pressure ? "pressure " : "")
           << "tiering range job " << job.job_id
           << " for camera " << range.camera_id
           << " [" << src_tier_cfg.tier << " -> " << dst_tier_cfg.tier << "]"
           << " range: " << range.start_time << "-" << range.end_time;
    return true;
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

    int64_t cutoff = static_cast<int64_t>(time(nullptr)) -
        static_cast<int64_t>(delete_policy.delete_after_days) * 86400;

    SegmentTierRangeImp range_imp;
    GET_CONFIG(int, max_ranges_per_cycle, Storage::kTieringMaxRangesPerCameraCycle);
    if (max_ranges_per_cycle <= 0)
        max_ranges_per_cycle = 64;
    auto ranges = range_imp.queryByCamera(camera_id, 0, cutoff, max_ranges_per_cycle);
    for (const auto &range : ranges) {
        if (range.end_time > cutoff)
            continue;
        if (range.status != segmentStatusToString(SegmentStatus::AVAILABLE))
            continue;
        if (isRangeDeletionProtected(range)) {
            continue;
        }

        if (delete_policy.delete_mode == deleteModeToString(DeleteMode::MARK_EXPIRED_WAIT_APPROVAL) ||
            delete_policy.require_approval_before_delete) {
            range_imp.updateStatusByRangeId(range.range_id, segmentStatusToString(SegmentStatus::EXPIRED));
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

    auto segments = collectSegmentFilesForRange(range);
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
        for (const auto &seg : segments) {
            if (seg.stream_id != range.stream_id) continue;
            auto key = TierObjectStorage::makeS3Key(base_path, seg.camera_id, seg.stream_id, seg.segment_path);
            // S3 DeleteObject is idempotent; a false result means an actual I/O failure.
            if (!_obj_storage.deleteSegment(pool.id, key))
                all_deleted = false;
        }
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
            if (!storage->deleteSegment(pool.id, seg.storage_key))
                all_deleted = false;
        }
    }

    if (!all_deleted) {
        StorageAlert alert;
        alert.id = StrUUID::make_guid(8, "alert");
        alert.level = "ERROR";
        alert.type = "TIER_DELETE_FAILED";
        alert.pool_id = range.pool_id;
        alert.message = "Failed to delete every segment for range " + range.range_id;
        alert.created_at = static_cast<int64_t>(time(nullptr));
        StorageAlertImp().add(alert);
        return false;
    }

    TierMetadataCleanupJobImp cleanup_imp;
    return cleanup_imp.markRangeDeletedAndEnqueue(
        range.range_id,
        range.camera_id,
        final_status.empty() ? segmentStatusToString(SegmentStatus::DELETED) : final_status);
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
    auto jobs = cleanup_imp.queryPending();
    for (const auto &job : jobs) {
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
            auto segments = collectSegmentFilesForRange(range);
            std::vector<TimeBlockIdentity> identities;
            for (const auto &segment : segments) {
                TimeBlockIdentity identity;
                identity.camera_id = segment.camera_id;
                identity.stream_id = segment.stream_id;
                identity.start_time = static_cast<uint64_t>(segment.start_time);
                identity.segment_path = segment.segment_path;
                identities.push_back(identity);
            }
            // TimeRebuilder swaps files asynchronously after active readers
            // drain. Do not run later metadata steps until a subsequent cycle
            // verifies that every TimeBlock identity for this range is gone.
            if (identities.empty()) {
                bool completed = cleanupRangeMotionMetadata(range) &&
                                 cleanupRangeBookmarkMetadata(range);
                cleanup_imp.updateResult(job.range_id, completed, completed ? "" : "Invalid metadata cleanup range");
                continue;
            }
            StorageManager::Instance().removeCameraTimeBlocks(range.camera_id, identities);
            cleanup_imp.updateResult(job.range_id, false, "Awaiting timefile swap verification");
        } catch (const std::exception &ex) {
            cleanup_imp.updateResult(job.range_id, false, ex.what());
        } catch (...) {
            cleanup_imp.updateResult(job.range_id, false, "Unknown metadata cleanup error");
        }
    }
}

bool TierStorageManager::cleanupRangeMotionMetadata(const SegmentTierRange &range) {
#if defined(ENABLE_MOTION)
    auto range_days = localDaysForWindow(range.start_time, range.end_time);
    if (range_days.empty())
        return false;

    MediaTuple tuple;
    tuple.app = range.camera_id;
    TimeQuery query(tuple, "", false);
    std::vector<uint64_t> empty_days;
    for (auto day_start : range_days) {
        auto day_end = nextLocalDayStart(day_start);
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
        int64_t max_cutoff = now - static_cast<int64_t>(params.option.keepArchivedMaxFor);
        SegmentTierRangeImp range_imp;
        GET_CONFIG(int, max_ranges_per_cycle, Storage::kTieringMaxRangesPerCameraCycle);
        if (max_ranges_per_cycle <= 0)
            max_ranges_per_cycle = 64;
        auto ranges = range_imp.queryByCamera(camera_id, 0, max_cutoff, max_ranges_per_cycle);
        for (const auto &range : ranges) {
            if (range.end_time > max_cutoff) continue;
            if (range.status != segmentStatusToString(SegmentStatus::AVAILABLE)) continue;
            expireRangeBestEffort(range);
        }
    }

    // keepArchivedMinFor keeps its original meaning: data newer than this
    // cutoff is protected from deletion under pressure. Pressure move is still
    // allowed because it preserves the recording in another tier.
    int64_t delete_protected_cutoff = params.option.keepArchivedMinForAuto
        ? now
        : now - static_cast<int64_t>(params.option.keepArchivedMinFor);

    std::vector<StoragePool> hot_warm_pools;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        for (const auto &kv : _pool_cache) {
            auto pool = kv.second;
            if (!pool.enabled) continue;
            if (pool.tier != tierTypeToString(HotTier) && pool.tier != tierTypeToString(WarmTier))
                continue;
            hot_warm_pools.push_back(pool);
        }
    }

    std::vector<StoragePool> pressured_pools;
    for (auto pool : hot_warm_pools) {
        fillPoolRuntimeStats(pool);
        if (pool.usage_pct >= static_cast<float>(pool.critical_watermark_percent))
            pressured_pools.push_back(pool);
    }

    if (pressured_pools.empty())
        return;

    SegmentTierRangeImp range_imp;
    GET_CONFIG(int, max_ranges_per_cycle, Storage::kTieringMaxRangesPerCameraCycle);
    if (max_ranges_per_cycle <= 0)
        max_ranges_per_cycle = 64;
    for (const auto &pool : pressured_pools) {
        auto ranges = range_imp.findByCameraTierPoolAndAge(
            camera_id, pool.tier, pool.id, delete_protected_cutoff,
            max_ranges_per_cycle);
        for (const auto &range : ranges) {
            if (range.end_time > delete_protected_cutoff) continue;
            if (range.status != segmentStatusToString(SegmentStatus::AVAILABLE)) continue;
            expireRangeBestEffort(range);
        }
    }
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
    if (max_concurrent <= 0)
        max_concurrent = 4;
    if (max_concurrent_per_pool <= 0)
        max_concurrent_per_pool = 2;

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
            ++state->active_by_pool[job.source_pool_id];
            if (!same_pool)
                ++state->active_by_pool[job.target_pool_id];
            --capacity;
            dispatch_jobs.push_back(job);
        }
    }

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    for (auto job : dispatch_jobs) {
        WorkThreadPool::Instance().getPoller()->async(
            [weak_self, state, job]() mutable {
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
                    state->completed.notify_all();
                }

                if (self && !self->_stopping)
                    self->dispatchPendingTieringJobs();
            });
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
    if (poolTypeIsObjectStorage(dst_pool.type)) {
        ok = executeObjectStorageUpload(job, dst_pool);
    } else {
        ok = executeLocalTierMove(job, src_pool, dst_pool);
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

    SegmentTierRange job_range;
    auto segments = collectSegmentFilesForJob(job, job_range);
    if (segments.empty()) {
        job.error_message = Optional<std::string>("No source segments found from tier ranges/timefile");
        WarnL << job.error_message.value() << " job=" << job.job_id;
        return false;
    }

    int64_t moved = 0;
    std::vector<std::string> copied_keys;
    std::vector<std::string> copied_paths;

    for (const auto &seg : segments) {
        if (seg.full_path.empty()) {
            WarnL << "Cannot resolve source segment path from pool_id=" << job.source_pool_id
                  << " segment=" << seg.segment_path;
            for (const auto &key : copied_keys) {
                WarnL << "Rollback copied segment after source resolve failure: pool=" << dst_pool.id
                      << " key=" << key;
                dst_storage->deleteSegment(dst_pool.id, key);
            }
            return false;
        }

        struct stat src_st{};
        if (stat(seg.full_path.c_str(), &src_st) != 0) {
            WarnL << "Cannot stat source segment: " << seg.full_path;
            for (const auto &key : copied_keys) {
                WarnL << "Rollback copied segment after source stat failure: pool=" << dst_pool.id
                      << " key=" << key;
                dst_storage->deleteSegment(dst_pool.id, key);
            }
            return false;
        }

        std::string dst_path = dst_storage->resolvePath(dst_pool.id, seg.storage_key);
        if (dst_path.empty()) {
            WarnL << "Cannot resolve destination segment path pool=" << dst_pool.id
                  << " key=" << seg.storage_key;
            for (const auto &key : copied_keys) {
                WarnL << "Rollback copied segment after destination resolve failure: pool=" << dst_pool.id
                      << " key=" << key;
                dst_storage->deleteSegment(dst_pool.id, key);
            }
            return false;
        }
        if (isSameFileOnDisk(seg.full_path, dst_path)) {
            WarnL << "Refuse to move segment because source and destination are the same file: "
                  << seg.full_path << " -> " << dst_path
                  << " source_pool=" << job.source_pool_id
                  << " target_pool=" << dst_pool.id;
            for (const auto &key : copied_keys) {
                WarnL << "Rollback copied segment after same-file detection: pool=" << dst_pool.id
                      << " key=" << key;
                dst_storage->deleteSegment(dst_pool.id, key);
            }
            return false;
        }

        if (!dst_storage->uploadSegment(dst_pool.id, seg.full_path, seg.storage_key)) {
            WarnL << "File storage upload failed for " << seg.full_path
                  << " -> " << dst_path
                  << " pool=" << dst_pool.id;
            for (const auto &key : copied_keys) {
                WarnL << "Rollback copied segment after upload failure: pool=" << dst_pool.id
                      << " key=" << key;
                dst_storage->deleteSegment(dst_pool.id, key);
            }
            return false;
        }

        copied_keys.push_back(seg.storage_key);
        copied_paths.push_back(dst_path);
        moved += src_st.st_size;
        InfoL << "Copied tier segment: " << seg.full_path
              << " -> " << dst_path
              << " bytes=" << src_st.st_size;
    }

    job.bytes_moved = moved;
    if (moved > 0) {
        SegmentTierRangeImp range_imp;
        if (!range_imp.updateTierByRangeId(job_range.range_id,
                                           job.target_tier,
                                           dst_pool.id,
                                           segmentStatusToString(SegmentStatus::AVAILABLE))) {
            job.error_message = Optional<std::string>("Failed to update tier range after local move");
            WarnL << job.error_message.value() << ", rollback copied files"
                  << " range=" << job_range.range_id
                  << " source_pool=" << job.source_pool_id
                  << " target_pool=" << dst_pool.id;
            for (const auto &key : copied_keys) {
                WarnL << "Rollback copied segment after DB update failure: pool=" << dst_pool.id
                      << " key=" << key;
                dst_storage->deleteSegment(dst_pool.id, key);
            }
            return false;
        }
        if (!range_imp.normalizeRangeById(job_range.range_id, 90, static_cast<int64_t>(time(nullptr)))) {
            WarnL << "Moved tier range but could not merge adjacent destination ranges: "
                  << job_range.range_id;
        }
    }

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto &seg = segments[i];
        const std::string dst_path = i < copied_paths.size() ? copied_paths[i] : "";
        if (isSameFileOnDisk(seg.full_path, dst_path)) {
            WarnL << "Skip deleting source because it matches destination: "
                  << seg.full_path << " -> " << dst_path;
            continue;
        }
        if (::remove(seg.full_path.c_str()) != 0) {
            WarnL << "Cannot remove old source segment after successful copy: " << seg.full_path
                  << ": " << strerror(errno);
            continue;
        }
        InfoL << "Moved segment: " << seg.full_path
              << " -> " << dst_path
              << " tier=" << dst_pool.tier;
    }
    return moved > 0;
}

// ---------------------------------------------------------------------------
// Object storage upload (MINIO / S3 / ARCHIVE)
// ---------------------------------------------------------------------------
bool TierStorageManager::executeObjectStorageUpload(TieringJob &job,
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

    SegmentTierRange job_range;
    auto segments = collectSegmentFilesForJob(job, job_range);

    if (segments.empty()) {
        job.error_message = std::string("No source segments found from tier ranges/timefile");
        WarnL << "executeObjectStorageUpload: " << job.error_message.value()
              << " job=" << job.job_id;
        return false;
    }

    int64_t bytes_moved = 0;
    std::vector<std::string> uploaded_keys;

    for (auto &seg : segments) {
        if (seg.full_path.empty()) {
            WarnL << "executeObjectStorageUpload: cannot resolve local path for segment "
                  << seg.segment_path << " pool_id=" << job.source_pool_id;
            for (const auto &key : uploaded_keys)
                _obj_storage.deleteSegment(dst_pool.id, key);
            return false;
        }

        struct stat st{};
        if (stat(seg.full_path.c_str(), &st) != 0) {
            WarnL << "executeObjectStorageUpload: segment file not found: " << seg.full_path;
            for (const auto &key : uploaded_keys)
                _obj_storage.deleteSegment(dst_pool.id, key);
            return false;
        }

        std::string s3_key = TierObjectStorage::makeS3Key(
            base_path, seg.camera_id, seg.stream_id, seg.segment_path);

        bool ok = _obj_storage.uploadSegment(dst_pool.id, seg.full_path, s3_key);
        if (!ok) {
            WarnL << "executeObjectStorageUpload: upload failed for " << seg.full_path;
            for (const auto &key : uploaded_keys)
                _obj_storage.deleteSegment(dst_pool.id, key);
            return false;
        }

        bytes_moved += static_cast<int64_t>(st.st_size);
        uploaded_keys.push_back(s3_key);
    }

    job.bytes_moved = bytes_moved;
    if (!uploaded_keys.empty()) {
        SegmentTierRangeImp range_imp;
        if (!range_imp.updateTierByRangeId(job_range.range_id,
                                           tierTypeToString(tierTypeFromString(job.target_tier)),
                                           dst_pool.id,
                                           segmentStatusToString(SegmentStatus::AVAILABLE))) {
            WarnL << "Failed to update tier range after object upload, range=" << job_range.range_id;
            for (const auto &key : uploaded_keys)
                _obj_storage.deleteSegment(dst_pool.id, key);
            return false;
        }
        if (!range_imp.normalizeRangeById(job_range.range_id, 90, static_cast<int64_t>(time(nullptr)))) {
            WarnL << "Uploaded tier range but could not merge adjacent destination ranges: "
                  << job_range.range_id;
        }
    }

    for (auto &seg : segments) {
        if (::unlink(seg.full_path.c_str()) != 0) {
            WarnL << "executeObjectStorageUpload: could not remove old local file "
                  << seg.full_path << ": " << strerror(errno);
            continue;
        }
    }

    InfoL << "executeObjectStorageUpload: " << uploaded_keys.size() << " segments, "
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
            if (file.start_time < start_time || file.start_time >= end_time)
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
    std::lock_guard<std::mutex> dispatch_lk(_restore_dispatch_mtx);
    GET_CONFIG(int, max_concurrent, Storage::kAutoRestoreMaxConcurrent);
    max_concurrent = std::max(1, max_concurrent);
    const int capacity = max_concurrent - _restore_inflight.load();
    if (capacity <= 0)
        return;
    auto children = RestoreJobSegmentImp().claimPending(capacity);
    for (const auto &child : children) {
        ++_restore_inflight;
        RestoreJobImp().updateStatus(child.job_id, "RUNNING");
        weak_ptr<TierStorageManager> weak_self = shared_from_this();
        WorkThreadPool::Instance().getExecutor()->async([weak_self, child]() {
            auto self = weak_self.lock();
            if (!self) return;
            {
                struct RestoreGuard {
                    std::atomic<int> &inflight;
                    explicit RestoreGuard(std::atomic<int> &value) : inflight(value) {}
                    ~RestoreGuard() { --inflight; }
                } guard(self->_restore_inflight);
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
            self->processRestoreJobs();
        });
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
    std::string s3_key = TierObjectStorage::makeS3Key(base_path, segment.camera_id, segment.stream_id, segment.segment_path);
    std::string directory_error;
    if (!prepareRestoreJobDirectory(segment.job_id, directory_error)) {
        segment_imp.updateStatus(segment, "FAILED", 0, directory_error);
        updateRestoreParent(segment.job_id);
        return;
    }
    std::string restore_path = buildRestoreSegmentPath(
        segment.job_id, segment.camera_id, segment.stream_id, segment.segment_path);
    std::string restore_dir = restore_path.substr(0, restore_path.rfind('/'));
    std::string tmp_path = restore_path + ".part";

    std::string mkdir_err;
    if (!ensureDirectoryPath(restore_dir, S_IRWXO | S_IRWXG | S_IRWXU, mkdir_err)) {
        segment_imp.updateStatus(segment, "FAILED", 0, mkdir_err);
        updateRestoreParent(segment.job_id);
        return;
    }

    struct stat st{};
    if (stat(restore_path.c_str(), &st) == 0) {
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

    bool ok = _obj_storage.downloadSegment(source_pool.id, s3_key, tmp_path);
    if (!ok) {
        segment_imp.updateStatus(segment, "FAILED", 0, "Download from object storage failed");
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
            if (stats.is_online) {
                pool.capacity_available = stats.capacity_available;
                pool.capacity_status = stats.capacity_status;
                pool.capacity_message = stats.capacity_message;
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
    WorkThreadPool::Instance().getPoller()->async([weak_self, cycle_state]() {
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
    });
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
        pools_arr.append(p.toJson());
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
void TierStorageManager::pruneOldMetrics() {
    // Keep 30 days of metrics
    int64_t cutoff = static_cast<int64_t>(time(nullptr)) - 30LL * 86400;
    PoolMetricsImp imp;
    imp.pruneOlderThan(cutoff);

    GET_CONFIG(int64_t, tiering_job_retention_days, Storage::kTieringJobRetentionDays);
    if (tiering_job_retention_days > 0) {
        TieringJobImp().pruneTerminalOlderThan(static_cast<int64_t>(time(nullptr)) - tiering_job_retention_days * 86400);
    }

    GET_CONFIG(int64_t, terminal_range_retention_days, Storage::kTieringTerminalRangeRetentionDays);
    if (terminal_range_retention_days > 0) {
        auto terminal_cutoff = static_cast<int64_t>(time(nullptr)) - terminal_range_retention_days * 86400;
        SegmentTierRangeImp().pruneDeletedOlderThan(terminal_cutoff);
        TierMetadataCleanupJobImp().pruneDoneOlderThan(terminal_cutoff);
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
                            query = std::make_shared<TimeQuery>(tuple, "", false);
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

static void* s_tag;

static onceToken g_token(
[]() {
#ifdef ENABLE_MP4
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        int64_t start_time = static_cast<int64_t>(info.start_time);
        int64_t end_time = start_time + std::max<int64_t>(1, static_cast<int64_t>(std::round(info.time_len)));
        auto origin_stream = StrMediaUtils::originStream(info.stream);
        TierStorageManager::Instance().registerHotSegmentRange(info.app, origin_stream, info.file_path, start_time, end_time, static_cast<int64_t>(info.file_size));
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
