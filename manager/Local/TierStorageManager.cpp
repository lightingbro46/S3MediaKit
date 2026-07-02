#include <ctime>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "Util/util.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Common/StrUtil.h"
#include "Thread/WorkThreadPool.h"
#include "Server/GlobalMonitor.h"
#include "Local/StatisticRecorder.h"

#include "TierStorageManager.h"
#include "StorageManager.h"
#include "Storage/StorageTierExtra.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(TierStorageManager)

namespace {

static const char kDefaultHotPoolName[] = "Default HOT Tier Storage";

struct DiskSegmentInfo {
    std::string camera_id;
    std::string stream_id;
    std::string segment_path;
    std::string full_path;
    int64_t start_time = 0;
    int64_t end_time = 0;
    int64_t file_size = 0;
};

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

        struct stat st{};
        DiskSegmentInfo item;
        item.camera_id = camera_id;
        item.stream_id = stream_id;
        item.segment_path = segment_path;
        item.full_path = path;
        item.start_time = ts;
        item.end_time = seg_end;
        item.file_size = (stat(path.c_str(), &st) == 0) ? static_cast<int64_t>(st.st_size) : 0;
        ret.push_back(std::move(item));
        return true;
    }, false, true);

    std::sort(ret.begin(), ret.end(), [](const DiskSegmentInfo &a, const DiskSegmentInfo &b) {
        return a.start_time < b.start_time;
    });
    return ret;
}

static std::vector<DiskSegmentInfo> collectDiskSegments(const std::string &camera_id,
                                                        int64_t start_time,
                                                        int64_t end_time) {
    GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    return collectDiskSegmentsFromRoot(File::absolutePath(app_name, mp4_save_path),
                                       camera_id,
                                       start_time,
                                       end_time);
}

static bool parseRecordFilePath(const std::string &file_path,
                                std::string &camera_id,
                                std::string &stream_id,
                                std::string &segment_path,
                                int64_t &segment_start) {
    GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    std::string rec_root = File::absolutePath(app_name, mp4_save_path);
    if (!start_with(file_path, rec_root))
        return false;

    std::string rel = file_path.substr(rec_root.size());
    if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    auto first = rel.find('/');
    if (first == std::string::npos)
        return false;
    auto second = rel.find('/', first + 1);
    if (second == std::string::npos)
        return false;

    camera_id = rel.substr(0, first);
    stream_id = rel.substr(first + 1, second - first - 1);
    segment_path = rel.substr(second + 1);
    if (!end_with(segment_path, ".mp4"))
        return false;
    segment_path.resize(segment_path.size() - 4);

    segment_start = static_cast<int64_t>(StrTimeUtils::getTsFromDateTimeStr(segment_path));
    if (segment_start <= 0)
        segment_start = static_cast<int64_t>(StrTimeUtils::getTsFromDateTimeStr2(segment_path));
    return segment_start > 0;
}

static std::string getRestoreRootPath() {
    GET_CONFIG(std::string, restore_save_path, Storage::kRestoreSavePath);
    return File::absolutePath("", restore_save_path);
}

static std::string buildRestoreSegmentPath(const std::string &camera_id,
                                           const std::string &stream_id,
                                           const std::string &segment_path) {
    auto root = getRestoreRootPath();
    if (!root.empty() && root.back() == '/')
        root.pop_back();
    return root + "/" + camera_id + "/" + stream_id + "/" + segment_path + ".mp4";
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
        WarnL << "registerPoolObjStorage: missing credentials for pool " << pool.id;
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

    std::string path = pool.mount_path.value_or(pool.network_path.value_or(""));
    if (path.empty()) {
        WarnL << "registerPoolFileStorage: missing mount_path/network_path for pool " << pool.id;
        return false;
    }

    return storage->registerPool(pool.id, pool.type, path);
}

void TierStorageManager::unregisterPoolFileStorage(const std::string &pool_id) {
    _local_storage.unregisterPool(pool_id);
    _nas_storage.unregisterPool(pool_id);
}

void TierStorageManager::ensureDefaultHotPool() {
    GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    std::string record_path = File::absolutePath(app_name, mp4_save_path);
    if (record_path.empty()) {
        WarnL << "ensureDefaultHotPool: Cannot create default HOT pool, kMP4SavePath is empty";
        return;
    }
    if (!File::is_dir(record_path)) {
        auto created = File::create_path(record_path, 0755);
        if (!created) {
            WarnL << "ensureDefaultHotPool: Cannot create record folder, kMP4SavePath is not writable: " << record_path;
            return;
        }
        DebugL << "Created record folder: " << record_path;
    }

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
    auto hot_pools = imp.queryAll(tierTypeToString(HotTier));
    if (!hot_pools.empty()) {
        InfoL << "TierStorageManager: HOT pool already configured, skip default HOT pool";
        return;
    }

    {
        StoragePool pool;
        pool.name = kDefaultHotPoolName;
        pool.type = poolTypeToString(StoragePoolType::LOCAL_DISK);
        pool.tier = tierTypeToString(HotTier);
        pool.mount_path = Optional<std::string>(record_path);
        pool.enabled = 1;
        pool.health_check_enabled = 1;
        pool.high_watermark_percent = auto_hot_high_watermark;
        pool.critical_watermark_percent = auto_hot_critical_watermark;
        if (createPool(pool).empty()) {
            WarnL << "TierStorageManager: failed to create default HOT pool at " << record_path;
            return;
        }
        InfoL << "TierStorageManager: created default HOT pool " << pool.id << " path=" << record_path;
    }

    // todo: create system default policy that uses this HOT pool as the only tier
}

// ===================================================================
// Lifecycle
// ===================================================================
void TierStorageManager::start() {
    if (_timer) {
        WarnL << "TierStorageManager already running. Ignore";
        return;
    }

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

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    // Run every 5 minutes (300 s)
    _timer = std::make_shared<Timer>(
        300.0f,
        [weak_self]() -> bool {
            auto self = weak_self.lock();
            if (!self) return false;
            self->checkAndRecordPoolHealth();
            self->runTieringCycle();
            self->pruneOldMetrics();
            self->cleanupRestoreTempFiles();
            return true;
        },
        _poller);

    InfoL << "TierStorageManager started";
}

void TierStorageManager::stop() {
    _timer.reset();
    InfoL << "TierStorageManager stopped";
}

// ===================================================================
// ID generation
// ===================================================================
std::string TierStorageManager::generateId(const std::string &prefix) {
    auto id = format_guid(strToLower(makeRandStr(32)));
    return prefix.empty() ? id : (prefix + "-" + id.substr(0, 8));
}

// ===================================================================
// Section 1 — Storage Pool APIs
// ===================================================================
std::string TierStorageManager::createPool(StoragePool pool) {
    if (pool.id.empty())
        pool.id = generateId("pool");
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

    if (pool.enabled) {
        if (poolTypeIsObjectStorage(pool.type))
            registerPoolObjStorage(pool);
        else
            registerPoolFileStorage(pool);
    }

    InfoL << "Created storage pool: " << pool.id << " (" << pool.name << ")";
    return pool.id;
}

bool TierStorageManager::updatePool(const StoragePool &pool) {
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
    return true;
}

bool TierStorageManager::deletePool(const std::string &pool_id, int &out_ref_count) {
    StoragePoolImp imp;
    out_ref_count = imp.countPolicyReferences(pool_id);
    if (out_ref_count > 0) {
        WarnL << "Cannot delete pool " << pool_id << ": referenced by " << out_ref_count << " policies";
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
    unregisterPoolObjStorage(pool_id);
    unregisterPoolFileStorage(pool_id);
    return true;
}

std::vector<StoragePool> TierStorageManager::listPools(const std::string &tier,
                                                        const std::string &type,
                                                        const std::string &keyword) {
    StoragePoolImp imp;
    auto pools = imp.queryAll(tier, type, keyword);
    // Enrich with live health stats
    for (auto &p : pools) fillPoolRuntimeStats(p);
    return pools;
}

std::vector<StoragePool> TierStorageManager::getPool(const std::string &pool_id) {
    StoragePoolImp imp;
    auto pools = imp.findByPoolId(pool_id);
    for (auto &p : pools) fillPoolRuntimeStats(p);
    return pools;
}

bool TierStorageManager::testPoolConnection(const StoragePool &pool, std::string &out_message, int &out_latency_ms) {
    if (poolTypeIsObjectStorage(pool.type)) {
        // If the pool is already registered (loaded at startup), test via its client.
        if (_obj_storage.isRegistered(pool.id))
            return _obj_storage.testConnection(pool.id, out_message, out_latency_ms);

        // Otherwise do an ad-hoc test using the caller-provided credentials.
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

    std::string path = pool.mount_path.value_or(pool.network_path.value_or(""));
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

std::vector<DiskPartition> TierStorageManager::getAvailableMountPoints(const string &include_types) {
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

    std::vector<DiskPartition> ret;
    GET_CONFIG(string, app_name, Record::kAppName);
    for (const auto &hdd : hdd_usage) {
        StoragePoolType pool_type = hdd.isNetworkFileSystem() ? StoragePoolType::NAS : StoragePoolType::LOCAL_DISK;
        auto storage = fileStorageForPoolType(poolTypeToString(pool_type));
        if (!storage) {
            continue;
        }
        auto record_path = File::absolutePath(app_name, hdd.mount_point);
        if (!File::create_path(record_path, 0755)) { // ensure writable
            WarnL << "Mount point " << hdd.mount_point << " is not writable for app: " << app_name;
            continue;
        }

        if (!isIncluded(poolTypeToString(pool_type))) {
            WarnL << "Mount point " << hdd.mount_point << " is not included in the requested storage types: " << include_types;
            continue; // skip if the caller explicitly included this type
        }

        bool already_used = false;
        for (const auto &pool : local_pools) {
            std::string pool_path = pool.mount_path.value_or("");
            if (!pool_path.empty() && pool_path == record_path) {
                already_used = true;
                break;
            }
            if (!pool_path.empty()) {
                std::string mount_point_origin = GlobalMonitor::Instance().findMountPoint(pool_path);
                if (mount_point_origin == hdd.mount_point) {
                    already_used = true;
                    break;
                }
            }
        }
        for (const auto &pool : nas_pools) {
            std::string pool_path = pool.mount_path.value_or("");
            if (!pool_path.empty() && pool_path == record_path) {
                already_used = true;
                break;
            }
            if (!pool_path.empty()) {
                std::string mount_point_origin = GlobalMonitor::Instance().findMountPoint(pool_path);
                if (mount_point_origin == hdd.mount_point) {
                    already_used = true;
                    break;
                }
            }
        }
        if (!already_used) {
            DebugL << "Available mount point: " << hdd.mount_point << " type=" << poolTypeToString(pool_type) << " fs=" << hdd.filesystem_type;
            DiskPartition hdd_copy = hdd; // copy to avoid modifying the original
            hdd_copy.mount_point = record_path; // use absolute path for the app
            ret.push_back(std::move(hdd_copy));
        }
    }
    return ret;
}

// ===================================================================
// Section 2 — Storage Policy APIs
// ===================================================================
std::string TierStorageManager::createPolicy(StoragePolicy policy) {
    if (policy.id.empty())
        policy.id = generateId("pol");
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
    StoragePolicy updated = policy;
    updated.updated_at = static_cast<int64_t>(time(nullptr));

    if (updated.tiers_json.empty())        updated.tiers_json        = "[]";
    if (updated.delete_policy_json.empty()) updated.delete_policy_json = "{}";
    if (updated.advanced_rules_json.empty()) updated.advanced_rules_json = "{}";

    StoragePolicyImp imp;
    return imp.update(updated);
}

bool TierStorageManager::deletePolicy(const std::string &policy_id, int &out_camera_count) {
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
    cloned.id             = generateId("pol");
    cloned.name           = new_name;
    cloned.created_at     = static_cast<int64_t>(time(nullptr));
    cloned.updated_at     = cloned.created_at;

    StoragePolicyImp imp;
    if (!imp.add(cloned)) {
        WarnL << "Failed to clone policy: " << policy_id;
        return "";
    }
    return cloned.id;
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

    CameraPolicyAssignment a;
    a.camera_id     = camera_id;
    a.policy_id     = policy_id;
    a.override_reason = reason.empty() ? Optional<std::string>() : Optional<std::string>(reason);
    a.assigned_at   = static_cast<int64_t>(time(nullptr));

    PolicyAssignmentImp imp;
    return imp.assign(a);
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
    return imp.remove(camera_id);
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

    // 2. System default (first enabled policy)
    {
        std::lock_guard<std::mutex> lk(_default_policy_mtx);
        if (!_default_policy_id.empty()) {
            result.policy_id = _default_policy_id;
            result.source    = policySourceToString(PolicySource::SYSTEM_DEFAULT);
            auto policies = getPolicy(result.policy_id);
            if (!policies.empty()) {
                result.policy_name           = policies.front().name;
                result.allow_camera_override = (policies.front().allow_camera_override != 0);
            }
            return result;
        }
    }

    // No policy available
    result.source = policySourceToString(PolicySource::SYSTEM_DEFAULT);
    return result;
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
        tr.status = r.status.empty() ? segmentStatusToString(SegmentStatus::AVAILABLE) : r.status;
        result.push_back(tr);
    }
    if (!result.empty())
        return result;

    SegmentTierImp imp;
    auto records = imp.findByCamera(camera_id, start_time, end_time);

    result.reserve(records.size());
    for (const auto &r : records) {
        TimelineRange tr;
        tr.start  = r.start_time;
        tr.end    = r.end_time;
        tr.tier   = r.tier;
        tr.status = r.status.empty() ? segmentStatusToString(SegmentStatus::AVAILABLE) : r.status;
        result.push_back(tr);
    }

    // If no DB records exist for this window, fall back to HOT (raw filesystem)
    // and generate timeline ranges filled with HOT tier info.
    if (result.empty()) {
        // Provide a single "HOT" range to indicate data may exist on local disk
        if (start_time < end_time) {
            TimelineRange tr;
            tr.start  = start_time;
            tr.end    = end_time;
            tr.tier   = tierTypeToString(HotTier);
            tr.status = segmentStatusToString(SegmentStatus::AVAILABLE);
            result.push_back(tr);
        }
    }
    return result;
}

CameraStorageSummary TierStorageManager::getCameraStorageSummary(const std::string &camera_id) {
    CameraStorageSummary summary;
    summary.camera_id = camera_id;

    SegmentTierRangeImp range_imp;
    auto ranges = range_imp.queryByCamera(camera_id);

    std::unordered_map<std::string, TierSummaryInfo> tier_map;
    for (const auto &r : ranges) {
        auto &ti = tier_map[r.tier];
        ti.tier = r.tier;
        ti.pool_id = r.pool_id.value_or("");
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

    if (!ranges.empty()) {
        for (auto &kv : tier_map)
            summary.tiers.push_back(std::move(kv.second));
        return summary;
    }

    SegmentTierImp imp;
    auto records = imp.findByCamera(camera_id);

    // Aggregate per tier
    for (const auto &r : records) {
        auto &ti = tier_map[r.tier];
        ti.tier        = r.tier;
        ti.pool_id     = r.pool_id.value_or("");
        ti.used_bytes  += r.file_size;
        ti.segment_count++;
        if (ti.oldest_segment_time == 0 || r.start_time < ti.oldest_segment_time)
            ti.oldest_segment_time = r.start_time;
        if (r.end_time > ti.newest_segment_time)
            ti.newest_segment_time = r.end_time;

        summary.total_used_bytes += r.file_size;
        summary.total_segments++;
        if (summary.oldest_time == 0 || r.start_time < summary.oldest_time)
            summary.oldest_time = r.start_time;
        if (r.end_time > summary.newest_time)
            summary.newest_time = r.end_time;
    }

    for (auto &kv : tier_map)
        summary.tiers.push_back(std::move(kv.second));

    return summary;
}

// ===================================================================
// Tiering engine
// ===================================================================
void TierStorageManager::runTieringCycle() {
    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self]() {
        auto self = weak_self.lock();
        if (!self) return;

        self->_ticker.resetTime();
        InfoL << "TierStorageManager: starting tiering cycle";

        // Refresh pool cache
        {
            StoragePoolImp pool_imp;
            auto all_pools = pool_imp.queryAll();
            std::lock_guard<std::mutex> lk(self->_pool_cache_mtx);
            self->_pool_cache.clear();
            for (const auto &p : all_pools)
                self->_pool_cache[p.id] = p;
        }

        // Refresh default policy
        {
            StoragePolicyImp pol_imp;
            auto enabled = pol_imp.queryAll("", 1, 0, 1);
            std::lock_guard<std::mutex> lk(self->_default_policy_mtx);
            self->_default_policy_id = enabled.empty() ? "" : enabled.front().id;
        }

        // // Register local recording files as compact HOT ranges before tiering.
        // todo:
        // {
        //     GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
        //     GET_CONFIG(std::string, app_name, Record::kAppName);
        //     std::string rec_root = File::absolutePath(app_name, mp4_save_path);

        //     File::scanDir(rec_root, [self](const std::string &camera_root, bool isDir) {
        //         if (!isDir)
        //             return true;
        //         auto slash = camera_root.find_last_of('/');
        //         if (slash == std::string::npos)
        //             return true;
        //         std::string camera_id = camera_root.substr(slash + 1);
        //         if (camera_id.empty())
        //             return true;

        //         File::scanDir(camera_root, [self, camera_id](const std::string &path, bool isDir) {
        //             if (!isDir)
        //                 return true;
        //             auto slash = path.find_last_of('/');
        //             if (slash == std::string::npos)
        //                 return true;
        //             std::string stream_id = path.substr(slash + 1);
        //             if (!stream_id.empty())
        //                 self->syncSegmentRecords(camera_id, stream_id, path);
        //             return true;
        //         }, false, false);
        //         return true;
        //     }, false, false);
        // }

        // Execute pending jobs first (pick up where we left off)
        TieringJobImp job_imp;
        auto pending = job_imp.findByStatus(jobStatusToString(JobStatus::PENDING));
        for (auto &job : pending) {
            self->executePendingJob(job);
        }

        // Gather cameras from compact ranges first; fallback to legacy records.
        SegmentTierRangeImp range_imp;
        auto camera_ids = range_imp.findDistinctAvailableCameras();
        if (camera_ids.empty()) {
            SegmentTierImp seg_imp;
            camera_ids = seg_imp.findDistinctAvailableCameras();
        }

        for (const auto &cam_id : camera_ids) {
            try {
                auto eff = self->getEffectivePolicy(cam_id);
                if (!eff.policy_id.empty()) {
                    auto policies = self->getPolicy(eff.policy_id);
                    if (!policies.empty()) {
                        self->processCameraTiering(cam_id, policies.front());
                        self->processCameraPressureTiering(cam_id, policies.front());
                    }
                }
                self->enforceCameraArchiveRetention(cam_id);
            } catch (const std::exception &e) {
                WarnL << "TierStorageManager: error processing camera " << cam_id << ": " << e.what();
            }
        }

        InfoL << "TierStorageManager: tiering cycle done. Elapsed: " << format_duration_verbose(self->_ticker.elapsedTime());
    });
}

void TierStorageManager::processCameraTiering(const std::string &camera_id,
                                               const StoragePolicy &policy) {
    auto tiers = policy.parsedTiers();
    auto rules = policy.parsedAdvancedRules();
    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t min_age_secs = static_cast<int64_t>(rules.min_segment_age_minutes_before_move) * 60;

    // Sort tiers: HOT first, then WARM, then COLD
    std::sort(tiers.begin(), tiers.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
        return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
    });

    SegmentTierRangeImp range_imp;
    SegmentTierImp seg_imp;
    TieringJobImp  job_imp;

    for (size_t i = 0; i + 1 < tiers.size(); ++i) {
        const auto &src_tier_cfg = tiers[i];
        const auto &dst_tier_cfg = tiers[i + 1];
        if (!src_tier_cfg.enabled || !dst_tier_cfg.enabled) continue;
        if (dst_tier_cfg.pool_id.empty()) continue;

        // Age threshold: segments whose end_time is before this should move
        int64_t move_threshold = now - (static_cast<int64_t>(src_tier_cfg.retain_until_days) * 86400);

        // Respect min_segment_age
        if (move_threshold > now - min_age_secs)
            move_threshold = now - min_age_secs;

        auto ranges = range_imp.findByTierAndAge(src_tier_cfg.tier, move_threshold);

        for (const auto &range : ranges) {
            if (range.camera_id != camera_id) continue;
            queueTierMoveJob(range, src_tier_cfg, dst_tier_cfg, false);
        }

        if (!ranges.empty())
            continue;

        // fallback to legacy segment records if no compact ranges exist
        auto segments = seg_imp.findByTierAndAge(src_tier_cfg.tier, move_threshold);

        for (const auto &seg : segments) {
            if (seg.camera_id != camera_id) continue;

            // Check for existing pending/running job for this segment
            auto existing_jobs = job_imp.query(camera_id, "", src_tier_cfg.tier, dst_tier_cfg.tier);
            bool already_queued = false;
            for (const auto &j : existing_jobs) {
                if (j.segment_start_time == seg.start_time &&
                    (j.status == jobStatusToString(JobStatus::PENDING) ||
                     j.status == jobStatusToString(JobStatus::RUNNING))) {
                    already_queued = true;
                    break;
                }
            }
            if (already_queued) continue;

            // Create a new tiering job
            TieringJob job;
            job.job_id             = generateId("tj");
            job.camera_id          = camera_id;
            job.source_tier        = src_tier_cfg.tier;
            job.target_tier        = dst_tier_cfg.tier;
            job.source_pool_id     = src_tier_cfg.pool_id;
            job.target_pool_id     = dst_tier_cfg.pool_id;
            job.status             = jobStatusToString(JobStatus::PENDING);
            job.segment_start_time = seg.start_time;
            job.segment_end_time   = seg.end_time;
            job.bytes_total        = seg.file_size;
            job.created_at         = now;
            job.updated_at         = now;

            if (job_imp.add(job)) {
                DebugL << "Queued tiering job " << job.job_id
                       << " for camera " << camera_id
                       << " [" << src_tier_cfg.tier << " -> " << dst_tier_cfg.tier << "]"
                       << " segment: " << seg.segment_path;
                executePendingJob(job);
            }
        }
    }
}

bool TierStorageManager::queueTierMoveJob(const SegmentTierRange &range,
                                          const PolicyTierConfig &src_tier_cfg,
                                          const PolicyTierConfig &dst_tier_cfg,
                                          bool pressure) {
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
    job.job_id             = generateId(pressure ? "ptj" : "tj");
    job.camera_id          = range.camera_id;
    job.source_tier        = src_tier_cfg.tier;
    job.target_tier        = dst_tier_cfg.tier;
    job.source_pool_id     = range.pool_id.value_or(src_tier_cfg.pool_id);
    job.target_pool_id     = dst_tier_cfg.pool_id;
    job.status             = jobStatusToString(JobStatus::PENDING);
    job.segment_start_time = range.start_time;
    job.segment_end_time   = range.end_time;
    job.bytes_total        = range.size_bytes;
    job.created_at         = now;
    job.updated_at         = now;

    if (!job_imp.add(job))
        return false;

    DebugL << "Queued " << (pressure ? "pressure " : "")
           << "tiering range job " << job.job_id
           << " for camera " << range.camera_id
           << " [" << src_tier_cfg.tier << " -> " << dst_tier_cfg.tier << "]"
           << " range: " << range.start_time << "-" << range.end_time;
    executePendingJob(job);
    return true;
}

void TierStorageManager::processCameraPressureTiering(const std::string &camera_id,
                                                      const StoragePolicy &policy) {
    auto tiers = policy.parsedTiers();
    auto rules = policy.parsedAdvancedRules();
    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t min_age_secs = static_cast<int64_t>(rules.min_segment_age_minutes_before_move) * 60;
    int64_t stable_threshold = now - std::max<int64_t>(min_age_secs, 60);

    std::sort(tiers.begin(), tiers.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
        return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
    });

    SegmentTierRangeImp range_imp;
    for (size_t i = 0; i + 1 < tiers.size(); ++i) {
        const auto &src_tier_cfg = tiers[i];
        const auto &dst_tier_cfg = tiers[i + 1];
        if (!src_tier_cfg.enabled || !dst_tier_cfg.enabled) continue;
        if (dst_tier_cfg.pool_id.empty()) continue;

        StoragePool dst_pool;
        std::vector<StoragePool> src_pools;
        {
            std::lock_guard<std::mutex> lk(_pool_cache_mtx);
            auto dit = _pool_cache.find(dst_tier_cfg.pool_id);
            if (dit == _pool_cache.end())
                continue;
            dst_pool = dit->second;
            if (!src_tier_cfg.pool_id.empty()) {
                auto sit = _pool_cache.find(src_tier_cfg.pool_id);
                if (sit != _pool_cache.end())
                    src_pools.push_back(sit->second);
            } else {
                for (const auto &kv : _pool_cache) {
                    if (kv.second.enabled && kv.second.tier == src_tier_cfg.tier)
                        src_pools.push_back(kv.second);
                }
            }
        }

        fillPoolRuntimeStats(dst_pool);
        if (dst_pool.usage_pct >= static_cast<float>(dst_pool.critical_watermark_percent))
            continue;

        for (auto src_pool : src_pools) {
            fillPoolRuntimeStats(src_pool);
            if (src_pool.usage_pct < static_cast<float>(src_pool.high_watermark_percent))
                continue;

            auto ranges = range_imp.findByTierPoolAndAge(src_tier_cfg.tier, src_pool.id, stable_threshold);
            int64_t moved_bytes = 0;
            for (const auto &range : ranges) {
                if (range.camera_id != camera_id) continue;
                if (range.status != segmentStatusToString(SegmentStatus::AVAILABLE)) continue;

                PolicyTierConfig effective_src_cfg = src_tier_cfg;
                if (effective_src_cfg.pool_id.empty())
                    effective_src_cfg.pool_id = src_pool.id;

                if (queueTierMoveJob(range, effective_src_cfg, dst_tier_cfg, true)) {
                    moved_bytes += std::max<int64_t>(0, range.size_bytes);
                }

                // Hysteresis: reclaim roughly enough to go below high watermark by 5%.
                if (src_pool.total_bytes > 0 && moved_bytes > 0) {
                    float target_pct = std::max(0.0f, static_cast<float>(src_pool.high_watermark_percent) - 5.0f);
                    int64_t target_used = static_cast<int64_t>(src_pool.total_bytes * target_pct / 100.0f);
                    if (src_pool.used_bytes - moved_bytes <= target_used)
                        break;
                }
            }
        }
    }
}

bool TierStorageManager::expireRangeBestEffort(const SegmentTierRange &range) {
    StoragePool pool;
    bool has_pool = false;
    if (range.pool_id.has_value()) {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto it = _pool_cache.find(range.pool_id.value());
        if (it != _pool_cache.end()) {
            pool = it->second;
            has_pool = true;
        }
    }

    SegmentTierImp seg_imp;
    auto segments = seg_imp.findByCamera(range.camera_id, range.start_time, range.end_time);
    bool deleted_any = false;

    if (has_pool && poolTypeIsObjectStorage(pool.type)) {
        std::string base_path = pool.base_path.value_or("");
        if (!_obj_storage.isRegistered(pool.id))
            registerPoolObjStorage(pool);
        for (const auto &seg : segments) {
            if (seg.stream_id != range.stream_id) continue;
            auto key = TierObjectStorage::makeS3Key(base_path, seg.camera_id, seg.stream_id, seg.segment_path);
            if (_obj_storage.deleteSegment(pool.id, key))
                deleted_any = true;
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
            auto key = TierFileStorageBase::makeStorageKey(seg.camera_id, seg.stream_id, seg.segment_path);
            if (storage->deleteSegment(pool.id, key))
                deleted_any = true;
        }
        if (segments.empty()) {
            std::string base = pool.mount_path.value_or(pool.network_path.value_or(""));
            auto disk_segments = collectDiskSegmentsFromRoot(base, range.camera_id, range.start_time, range.end_time);
            for (const auto &seg : disk_segments) {
                if (seg.stream_id != range.stream_id) continue;
                auto key = TierFileStorageBase::makeStorageKey(seg.camera_id, seg.stream_id, seg.segment_path);
                if (storage->deleteSegment(pool.id, key))
                    deleted_any = true;
            }
        }
    } else {
        std::string base;
        if (base.empty()) {
            GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
            GET_CONFIG(std::string, app_name, Record::kAppName);
            base = File::absolutePath(app_name, mp4_save_path);
        }

        for (const auto &seg : segments) {
            if (seg.stream_id != range.stream_id) continue;
            std::string path = base + "/" + seg.camera_id + "/" + seg.stream_id + "/" + seg.segment_path + ".mp4";
            if (::unlink(path.c_str()) == 0)
                deleted_any = true;
        }

        if (segments.empty()) {
            auto disk_segments = collectDiskSegments(range.camera_id, range.start_time, range.end_time);
            for (const auto &seg : disk_segments) {
                if (seg.stream_id != range.stream_id) continue;
                if (::unlink(seg.full_path.c_str()) == 0)
                    deleted_any = true;
            }
        }
    }

    SegmentTierRangeImp range_imp;
    range_imp.updateStatusByRangeId(range.range_id, segmentStatusToString(SegmentStatus::EXPIRED));
    for (const auto &seg : segments) {
        if (seg.stream_id != range.stream_id) continue;
        seg_imp.updateStatus(seg.camera_id, seg.stream_id, seg.segment_path,
                             segmentStatusToString(SegmentStatus::EXPIRED));
    }
    notifyRebuildTimeFile(range.camera_id, static_cast<uint64_t>(range.end_time));
    return deleted_any;
}

void TierStorageManager::notifyRebuildTimeFile(const std::string &camera_id,
                                               uint64_t threshold) {
    if (camera_id.empty() || threshold == 0)
        return;
    NOTICE_EMIT(BroadcastRebuildTimeFileArgs,
                Broadcast::kBroadcastRebuildTimeFile,
                camera_id,
                threshold);
}

void TierStorageManager::enforceCameraArchiveRetention(const std::string &camera_id) {
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
        auto ranges = range_imp.queryByCamera(camera_id, 0, max_cutoff);
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
    for (const auto &pool : pressured_pools) {
        auto ranges = range_imp.findByTierPoolAndAge(pool.tier, pool.id, delete_protected_cutoff);
        for (const auto &range : ranges) {
            if (range.camera_id != camera_id) continue;
            if (range.end_time > delete_protected_cutoff) continue;
            if (range.status != segmentStatusToString(SegmentStatus::AVAILABLE)) continue;
            expireRangeBestEffort(range);
        }
    }
}

void TierStorageManager::executePendingJob(TieringJob &job) {
    TieringJobImp job_imp;
    job_imp.updateStatus(job.job_id, jobStatusToString(JobStatus::RUNNING));
    job.status = jobStatusToString(JobStatus::RUNNING);

    StoragePool dst_pool;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto it = _pool_cache.find(job.target_pool_id);
        if (it == _pool_cache.end()) {
            job_imp.updateStatus(job.job_id, jobStatusToString(JobStatus::FAILED),
                                 -1, "Target pool not found: " + job.target_pool_id);
            return;
        }
        dst_pool = it->second;
    }

    bool ok = false;
    if (poolTypeIsObjectStorage(dst_pool.type)) {
        ok = executeObjectStorageUpload(job, dst_pool);
    } else {
        StoragePool src_pool;
        {
            std::lock_guard<std::mutex> lk(_pool_cache_mtx);
            auto it = _pool_cache.find(job.source_pool_id);
            if (it != _pool_cache.end())
                src_pool = it->second;
        }
        ok = executeLocalTierMove(job, src_pool, dst_pool);
    }

    if (ok) {
        job_imp.updateStatus(job.job_id, jobStatusToString(JobStatus::DONE), job.bytes_moved);
        DebugL << "Tiering job done: " << job.job_id;
    } else {
        job_imp.updateStatus(job.job_id, jobStatusToString(JobStatus::FAILED),
                             job.bytes_moved, "Move failed");
        WarnL << "Tiering job failed: " << job.job_id;
    }
}

// ---------------------------------------------------------------------------
// Local filesystem move (LOCAL_DISK → NAS, or any mount-path–based move)
// ---------------------------------------------------------------------------
bool TierStorageManager::executeLocalTierMove(TieringJob &job,
                                               const StoragePool &src_pool,
                                               const StoragePool &dst_pool) {
    std::string dst_base = dst_pool.mount_path.value_or(dst_pool.network_path.value_or(""));
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

    bool source_pool_registered = false;
    TierFileStorageBase *src_storage = nullptr;
    if (!src_pool.id.empty() && !poolTypeIsObjectStorage(src_pool.type)) {
        src_storage = fileStorageForPoolType(src_pool.type);
        source_pool_registered = src_storage &&
            (src_storage->isRegistered(src_pool.id) || registerPoolFileStorage(src_pool));
    }

    // Find segments belonging to this job window
    SegmentTierImp seg_imp;
    auto segments = seg_imp.findByCamera(job.camera_id,
                                          job.segment_start_time,
                                          job.segment_end_time);
    std::string src_root = src_pool.mount_path.value_or(src_pool.network_path.value_or(""));
    if (segments.empty()) {
        std::vector<DiskSegmentInfo> disk_segments;
        if (!src_root.empty()) {
            disk_segments = collectDiskSegmentsFromRoot(src_root,
                                                        job.camera_id,
                                                        job.segment_start_time,
                                                        job.segment_end_time);
        } else {
            disk_segments = collectDiskSegments(job.camera_id,
                                                job.segment_start_time,
                                                job.segment_end_time);
        }
        for (const auto &d : disk_segments) {
            SegmentTierRecord seg;
            seg.camera_id = d.camera_id;
            seg.stream_id = d.stream_id;
            seg.segment_path = d.segment_path;
            seg.tier = job.source_tier;
            if (!job.source_pool_id.empty())
                seg.pool_id = Optional<std::string>(job.source_pool_id);
            seg.status = segmentStatusToString(SegmentStatus::AVAILABLE);
            seg.start_time = d.start_time;
            seg.end_time = d.end_time;
            seg.file_size = d.file_size;
            segments.push_back(std::move(seg));
        }
    }

    if (src_root.empty()) {
        GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath);
        GET_CONFIG(string, app_name,      Record::kAppName);
        src_root = File::absolutePath(app_name, mp4_save_path);
    }

    int64_t moved = 0;
    bool any_failed = false;

    for (const auto &seg : segments) {
        if (seg.tier != job.source_tier) continue;
        if (seg.status != segmentStatusToString(SegmentStatus::AVAILABLE)) continue;

        auto key = TierFileStorageBase::makeStorageKey(seg.camera_id, seg.stream_id, seg.segment_path);
        std::string src_full;
        if (source_pool_registered && seg.pool_id.has_value() && seg.pool_id.value() == src_pool.id) {
            src_full = src_storage->resolvePath(src_pool.id, key);
        } else if (seg.pool_id.has_value()) {
            StoragePool record_pool;
            bool found_record_pool = false;
            {
                std::lock_guard<std::mutex> lk(_pool_cache_mtx);
                auto pit = _pool_cache.find(seg.pool_id.value());
                if (pit != _pool_cache.end() && !poolTypeIsObjectStorage(pit->second.type)) {
                    record_pool = pit->second;
                    found_record_pool = true;
                }
            }
            auto record_storage = found_record_pool ? fileStorageForPoolType(record_pool.type) : nullptr;
            if (record_storage &&
                (record_storage->isRegistered(record_pool.id) || registerPoolFileStorage(record_pool))) {
                src_full = record_storage->resolvePath(record_pool.id, key);
            }
        }
        if (src_full.empty()) {
            src_full = src_root + "/" + seg.camera_id + "/" + seg.stream_id + "/"
                       + seg.segment_path + ".mp4";
        }

        struct stat st{};
        if (stat(src_full.c_str(), &st) != 0) {
            WarnL << "Cannot stat source segment: " << src_full;
            any_failed = true;
            continue;
        }

        if (!dst_storage->uploadSegment(dst_pool.id, src_full, key)) {
            WarnL << "File storage upload failed for " << src_full
                  << " -> pool " << dst_pool.id;
            any_failed = true;
            continue;
        }

        ::remove(src_full.c_str());
        seg_imp.updateTier(seg.camera_id, seg.stream_id, seg.segment_path,
                            job.target_tier, dst_pool.id,
                            segmentStatusToString(SegmentStatus::AVAILABLE));
        moved += st.st_size;
        DebugL << "Moved segment: " << seg.segment_path << " -> " << dst_pool.tier;
    }

    job.bytes_moved = moved;
    if (!any_failed && moved > 0) {
        SegmentTierRangeImp range_imp;
        range_imp.updateTierByWindow(job.camera_id,
                                     job.segment_start_time,
                                     job.segment_end_time,
                                     job.target_tier,
                                     dst_pool.id,
                                     segmentStatusToString(SegmentStatus::AVAILABLE));
        notifyRebuildTimeFile(job.camera_id, static_cast<uint64_t>(job.segment_end_time));
    }
    return !any_failed;
}

// ---------------------------------------------------------------------------
// Object storage upload (MINIO / S3 / ARCHIVE) — stub
// ---------------------------------------------------------------------------
bool TierStorageManager::executeObjectStorageUpload(TieringJob &job,
                                                     const StoragePool &dst_pool) {
    if (!_obj_storage.isRegistered(dst_pool.id)) {
        job.error_message = std::string("Pool not registered with object storage client: ")
                            + dst_pool.id;
        WarnL << job.error_message.value();
        return false;
    }

    const std::string base_path = dst_pool.base_path.has_value()
                                    ? dst_pool.base_path.value() : "";

    // Fetch segments that belong to this camera within the job's time window
    SegmentTierImp seg_imp;
    auto segments = seg_imp.findByCamera(
        job.camera_id,
        job.segment_start_time,
        job.segment_end_time);
    if (segments.empty()) {
        std::string source_root;
        {
            std::lock_guard<std::mutex> lk(_pool_cache_mtx);
            auto pit = _pool_cache.find(job.source_pool_id);
            if (pit != _pool_cache.end())
                source_root = pit->second.mount_path.value_or(pit->second.network_path.value_or(""));
        }

        std::vector<DiskSegmentInfo> disk_segments;
        if (!source_root.empty()) {
            disk_segments = collectDiskSegmentsFromRoot(source_root,
                                                        job.camera_id,
                                                        job.segment_start_time,
                                                        job.segment_end_time);
        } else {
            disk_segments = collectDiskSegments(job.camera_id,
                                                job.segment_start_time,
                                                job.segment_end_time);
        }
        for (const auto &d : disk_segments) {
            SegmentTierRecord seg;
            seg.camera_id = d.camera_id;
            seg.stream_id = d.stream_id;
            seg.segment_path = d.segment_path;
            seg.tier = job.source_tier;
            if (!job.source_pool_id.empty())
                seg.pool_id = Optional<std::string>(job.source_pool_id);
            seg.status = segmentStatusToString(SegmentStatus::AVAILABLE);
            seg.start_time = d.start_time;
            seg.end_time = d.end_time;
            seg.file_size = d.file_size;
            segments.push_back(std::move(seg));
        }
    }

    if (segments.empty()) {
        job.error_message = std::string("No segments found for upload window");
        WarnL << "executeObjectStorageUpload: " << job.error_message.value()
              << " job=" << job.job_id;
        return false;
    }

    int64_t bytes_moved = 0;
    int     uploaded    = 0;
    int     failed      = 0;

    for (auto &seg : segments) {
        // Only move segments currently on a local (source) tier
        if (seg.pool_id.has_value() && seg.pool_id.value() == dst_pool.id)
            continue; // already on target

        // Resolve the local file path from the segment record
        // segment_path stores the relative path under the stream directory
        // e.g. "2024-01-15/10-00-00"
        std::string local_path;
        {
            std::lock_guard<std::mutex> lk(_pool_cache_mtx);
            if (seg.pool_id.has_value()) {
                auto pit = _pool_cache.find(seg.pool_id.value());
                if (pit != _pool_cache.end()) {
                    std::string mount = pit->second.mount_path.value_or(
                                            pit->second.network_path.value_or(""));
                    if (!mount.empty()) {
                        if (mount.back() != '/') mount += '/';
                        local_path = mount + seg.camera_id + "/" + seg.stream_id + "/"
                                     + seg.segment_path + ".mp4";
                    }
                }
            }
        }

        if (local_path.empty()) {
            // Fallback: ask StorageManager for the recording root
            GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
            GET_CONFIG(std::string, app_name, Record::kAppName);
            std::string rec_root = File::absolutePath(app_name, mp4_save_path);
            if (!rec_root.empty()) {
                local_path = rec_root + '/' + seg.camera_id + '/'
                             + seg.stream_id + '/' + seg.segment_path + ".mp4";
            }
        }

        if (local_path.empty()) {
            WarnL << "executeObjectStorageUpload: cannot resolve local path for segment "
                  << seg.segment_path;
            ++failed;
            continue;
        }

        struct stat st{};
        if (stat(local_path.c_str(), &st) != 0) {
            WarnL << "executeObjectStorageUpload: segment file not found: " << local_path;
            ++failed;
            continue;
        }

        std::string s3_key = TierObjectStorage::makeS3Key(
            base_path, seg.camera_id, seg.stream_id, seg.segment_path);

        bool ok = _obj_storage.uploadSegment(dst_pool.id, local_path, s3_key);
        if (!ok) {
            WarnL << "executeObjectStorageUpload: upload failed for " << local_path;
            ++failed;
            continue;
        }

        // Update tier record to reflect new location
        seg_imp.updateTier(seg.camera_id, seg.stream_id, seg.segment_path,
                            tierTypeToString(tierTypeFromString(job.target_tier)),
                            dst_pool.id,
                            segmentStatusToString(SegmentStatus::AVAILABLE));

        bytes_moved += static_cast<int64_t>(st.st_size);
        ++uploaded;

        // Remove the local file once safely stored in object storage
        if (::unlink(local_path.c_str()) != 0) {
            WarnL << "executeObjectStorageUpload: could not remove local file "
                  << local_path << ": " << strerror(errno);
        }
    }

    job.bytes_moved = bytes_moved;
    if (uploaded > 0) {
        SegmentTierRangeImp range_imp;
        range_imp.updateTierByWindow(job.camera_id,
                                     job.segment_start_time,
                                     job.segment_end_time,
                                     tierTypeToString(tierTypeFromString(job.target_tier)),
                                     dst_pool.id,
                                     segmentStatusToString(SegmentStatus::AVAILABLE));
        notifyRebuildTimeFile(job.camera_id, static_cast<uint64_t>(job.segment_end_time));
    }

    if (failed > 0 && uploaded == 0) {
        job.error_message = std::string("All ") + std::to_string(failed)
                            + " segment uploads failed";
        return false;
    }
    if (failed > 0) {
        job.error_message = std::to_string(failed) + " of "
                            + std::to_string(uploaded + failed)
                            + " segment uploads failed (partial success)";
        // Return true so the job can be marked COMPLETED_PARTIAL
    }

    InfoL << "executeObjectStorageUpload: " << uploaded << " segments, "
          << bytes_moved << " bytes → pool " << dst_pool.id;
    return true;
}

ColdAccessRestoreResult TierStorageManager::handleColdAccessByPath(const std::string &file_path) {
    ColdAccessRestoreResult ret;
    GET_CONFIG(bool, auto_restore, Storage::kAutoRestoreOnRecordAccess);
    if (!auto_restore)
        return ret;

    std::string camera_id;
    std::string stream_id;
    std::string segment_path;
    int64_t segment_start = 0;
    if (!parseRecordFilePath(file_path, camera_id, stream_id, segment_path, segment_start))
        return ret;

    SegmentTierRangeImp range_imp;
    auto ranges = range_imp.queryByCamera(camera_id, segment_start, segment_start + 60);
    SegmentTierRange cold_range;
    bool found = false;
    for (const auto &r : ranges) {
        if (r.stream_id == stream_id &&
            r.tier == tierTypeToString(ColdTier) &&
            r.status != segmentStatusToString(SegmentStatus::DELETED) &&
            r.status != segmentStatusToString(SegmentStatus::EXPIRED)) {
            cold_range = r;
            found = true;
            break;
        }
    }
    if (!found || !cold_range.pool_id.has_value())
        return ret;

    RestoreJobImp restore_imp;
    for (const auto &status : std::vector<std::string>{"PENDING", "RUNNING"}) {
        auto jobs = restore_imp.query(camera_id, status, 0, 0, 0, 50);
        for (const auto &j : jobs) {
            if (j.start_time <= segment_start && j.end_time >= segment_start) {
                ret.handled = true;
                ret.job_id = j.job_id;
                ret.status = j.status;
                ret.message = "Restore job already queued";
                return ret;
            }
        }
    }

    StoragePool source_pool;
    {
        std::lock_guard<std::mutex> lk(_pool_cache_mtx);
        auto it = _pool_cache.find(cold_range.pool_id.value());
        if (it == _pool_cache.end())
            return ret;
        source_pool = it->second;
    }
    if (!poolTypeIsObjectStorage(source_pool.type))
        return ret;
    if (!_obj_storage.isRegistered(source_pool.id))
        registerPoolObjStorage(source_pool);

    int64_t now = static_cast<int64_t>(time(nullptr));
    RestoreJob job;
    job.job_id = generateId("restore");
    job.camera_id = camera_id;
    job.source_tier = tierTypeToString(ColdTier);
    job.target_tier = tierTypeToString(HotTier);
    job.status = "PENDING";
    job.start_time = segment_start;
    job.end_time = segment_start + 60;
    job.total_bytes = 0;
    job.processed_bytes = 0;
    job.reason = Optional<std::string>("AUTO_RECORD_ACCESS");
    job.created_at = now;
    job.updated_at = now;

    if (!restore_imp.add(job))
        return ret;

    int64_t segment_end = segment_start + 60;
    range_imp.splitWindowStatus(cold_range, segment_start, segment_end, segmentStatusToString(SegmentStatus::RESTORING));

    weak_ptr<TierStorageManager> weak_self = shared_from_this();
    std::string job_id = job.job_id;
    int64_t range_start = segment_start;
    int64_t range_end = segment_end;
    WorkThreadPool::Instance().getPoller()->async([weak_self, job_id, camera_id, stream_id,
                                                   segment_path, source_pool,
                                                   range_start, range_end]() {
        auto self = weak_self.lock();
        if (!self) return;
        self->executeRestoreSegment(job_id, camera_id, stream_id, segment_path,
                                    source_pool, range_start, range_end);
    });

    ret.handled = true;
    ret.job_id = job.job_id;
    ret.status = "PENDING";
    ret.message = "Segment is in COLD tier. Restore has been queued";
    return ret;
}

void TierStorageManager::executeRestoreSegment(const std::string &job_id,
                                               const std::string &camera_id,
                                               const std::string &stream_id,
                                               const std::string &segment_path,
                                               const StoragePool &source_pool,
                                               int64_t range_start,
                                               int64_t range_end) {
    RestoreJobImp restore_imp;
    SegmentTierRangeImp range_imp;
    restore_imp.updateStatus(job_id, "RUNNING");

    if (!_obj_storage.isRegistered(source_pool.id) && !registerPoolObjStorage(source_pool)) {
        restore_imp.updateStatus(job_id, "FAILED", -1, "Object storage pool is not registered");
        range_imp.updateStatusByExactWindow(camera_id, stream_id, range_start, range_end,
                                            segmentStatusToString(SegmentStatus::AVAILABLE));
        return;
    }

    std::string base_path = source_pool.base_path.value_or("");
    std::string s3_key = TierObjectStorage::makeS3Key(base_path, camera_id, stream_id, segment_path);
    std::string restore_path = buildRestoreSegmentPath(camera_id, stream_id, segment_path);
    std::string restore_dir = restore_path.substr(0, restore_path.rfind('/'));
    std::string tmp_path = restore_path + ".restore";

    File::create_path(restore_dir, 0755);

    struct stat st{};
    if (stat(restore_path.c_str(), &st) == 0) {
        int64_t restored_bytes = static_cast<int64_t>(st.st_size);
        restore_imp.updateStatus(job_id, "DONE", restored_bytes);
        range_imp.updateStatusByExactWindow(camera_id, stream_id, range_start, range_end,
                                            segmentStatusToString(SegmentStatus::AVAILABLE));
        return;
    }

    bool ok = _obj_storage.downloadSegment(source_pool.id, s3_key, tmp_path);
    if (!ok) {
        restore_imp.updateStatus(job_id, "FAILED", -1, "Download from object storage failed");
        range_imp.updateStatusByExactWindow(camera_id, stream_id, range_start, range_end,
                                            segmentStatusToString(SegmentStatus::AVAILABLE));
        ::unlink(tmp_path.c_str());
        return;
    }

    if (::rename(tmp_path.c_str(), restore_path.c_str()) != 0) {
        std::string err = std::string("Cannot move restored file: ") + strerror(errno);
        restore_imp.updateStatus(job_id, "FAILED", -1, err);
        range_imp.updateStatusByExactWindow(camera_id, stream_id, range_start, range_end,
                                            segmentStatusToString(SegmentStatus::AVAILABLE));
        ::unlink(tmp_path.c_str());
        return;
    }

    int64_t restored_bytes = (stat(restore_path.c_str(), &st) == 0) ? static_cast<int64_t>(st.st_size) : 0;
    restore_imp.updateStatus(job_id, "DONE", restored_bytes);
    range_imp.updateStatusByExactWindow(camera_id, stream_id, range_start, range_end,
                                        segmentStatusToString(SegmentStatus::AVAILABLE));
    InfoL << "Restored segment to temp cache: " << restore_path;
}

// ===================================================================
// Pool health monitoring
// ===================================================================
void TierStorageManager::fillPoolRuntimeStats(StoragePool &pool) {
    if (poolTypeIsObjectStorage(pool.type)) {
        // Try live Prometheus scrape / S3 HEAD first
        if (_obj_storage.isRegistered(pool.id)) {
            auto stats = _obj_storage.getPoolStats(pool.id);
            pool.last_health_check = static_cast<int64_t>(time(nullptr));

            if (stats.is_online) {
                pool.used_bytes  = static_cast<int64_t>(stats.used_bytes);
                pool.total_bytes = static_cast<int64_t>(stats.total_bytes);
                pool.usage_pct   = (stats.total_bytes > 0)
                    ? static_cast<float>(stats.used_bytes) * 100.f
                      / static_cast<float>(stats.total_bytes)
                    : 0.f;
                pool.health_status = healthToString(computePoolHealth(pool, pool.usage_pct));
                return;
            }
            // Client registered but offline
            pool.health_status = healthToString(StorageHealth::OFFLINE);
            return;
        }

        // Pool not yet registered (e.g. disabled) — fall back to last DB record
        PoolMetricsImp metrics_imp;
        auto m = metrics_imp.latestForPool(pool.id);
        pool.used_bytes        = m.used_bytes;
        pool.total_bytes       = m.total_bytes;
        pool.usage_pct         = m.usage_pct;
        pool.health_status     = m.health_status.empty()
                                   ? healthToString(StorageHealth::OK)
                                   : m.health_status;
        pool.last_health_check = m.timestamp;
        return;
    }

    auto file_storage = fileStorageForPoolType(pool.type);
    if (file_storage && !file_storage->isRegistered(pool.id))
        registerPoolFileStorage(pool);
    auto file_stats = file_storage ? file_storage->getPoolStats(pool.id) : TierPoolStats();
    if (file_stats.is_online) {
        pool.used_bytes  = static_cast<int64_t>(file_stats.used_bytes);
        pool.total_bytes = static_cast<int64_t>(file_stats.total_bytes);
        pool.usage_pct   = (file_stats.total_bytes > 0)
            ? static_cast<float>(file_stats.used_bytes) * 100.f
              / static_cast<float>(file_stats.total_bytes)
            : 0.f;
        pool.health_status = healthToString(computePoolHealth(pool, pool.usage_pct));
        pool.last_health_check = static_cast<int64_t>(time(nullptr));
        return;
    }

    // Fallback: local disk stats from GlobalMonitor.
    std::string path = pool.mount_path.value_or(pool.network_path.value_or(""));
    if (path.empty()) return;

    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    for (const auto &disk : hdd_usage) {
        if (toolkit::start_with(path, disk.mount_point)) {
            pool.used_bytes    = static_cast<int64_t>(disk.used_bytes);
            pool.total_bytes   = static_cast<int64_t>(disk.total_bytes);
            pool.usage_pct     = disk.usage_pct;
            auto health = computePoolHealth(pool, disk.usage_pct);
            pool.health_status = healthToString(health);
            pool.last_health_check = static_cast<int64_t>(time(nullptr));
            return;
        }
    }
    pool.health_status = healthToString(StorageHealth::OFFLINE);
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

    auto all_pools = pool_imp.queryAll("", "", "");
    int64_t now = static_cast<int64_t>(time(nullptr));

    for (auto &p : all_pools) {
        if (!p.enabled || !p.health_check_enabled) continue;
        fillPoolRuntimeStats(p);

        PoolMetrics m;
        m.pool_id       = p.id;
        m.timestamp     = now;
        m.used_bytes    = p.used_bytes;
        m.total_bytes   = p.total_bytes;
        m.usage_pct     = p.usage_pct;
        m.health_status = p.health_status;
        metrics_imp.record(m);

        // Refresh cache entry
        {
            std::lock_guard<std::mutex> lk(_pool_cache_mtx);
            auto it = _pool_cache.find(p.id);
            if (it != _pool_cache.end()) it->second = p;
        }

        DebugL << "Pool health: " << p.id << " [" << p.tier << "] " << p.health_status << " " << p.usage_pct << "%";
    }
}

// ===================================================================
// Tiering job queries (section 6 — exposed here for convenience)
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

bool TierStorageManager::cancelTieringJob(const std::string &job_id) {
    TieringJobImp imp;
    auto jobs = imp.findByJobId(job_id);
    if (jobs.empty()) return false;
    const auto &j = jobs.front();
    if (j.status == jobStatusToString(JobStatus::DONE) ||
        j.status == jobStatusToString(JobStatus::FAILED) ||
        j.status == jobStatusToString(JobStatus::CANCELLED))
        return false;
    return imp.updateStatus(job_id, jobStatusToString(JobStatus::CANCELLED));
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
void TierStorageManager::syncSegmentRecords(const std::string &camera_id,
                                             const std::string &stream_id,
                                             const std::string &stream_path) {
    SegmentTierImp imp;
    SegmentTierRangeImp range_imp;
    int64_t now = static_cast<int64_t>(time(nullptr));
    std::string hot_pool_id;
    // = getHotPoolIdForRecording();

    File::scanDir(stream_path, [&](const string &path, bool isDir) {
        if (isDir || !end_with(path, ".mp4")) return true;

        // Derive relative path from stream_path
        string rel = path.substr(stream_path.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        // Strip .mp4 suffix
        if (rel.size() > 4) rel = rel.substr(0, rel.size() - 4);

        auto start_ts = StrTimeUtils::getTsFromDateTimeStr(rel);
        if (start_ts <= 0)
            start_ts = StrTimeUtils::getTsFromDateTimeStr2(rel);
        if (start_ts <= 0)
            return true;

        struct stat st{};
        int64_t file_size = (stat(path.c_str(), &st) == 0) ? static_cast<int64_t>(st.st_size) : 0;
        int64_t start_time = static_cast<int64_t>(start_ts);
        int64_t end_time = start_time + 60;

        std::string hot_tier = tierTypeToString(HotTier);
        std::string available = segmentStatusToString(SegmentStatus::AVAILABLE);
        if (range_imp.hasCoveringRange(camera_id, stream_id, hot_tier, available, start_time, end_time))
            return true;

        SegmentTierRecord rec;
        rec.camera_id    = camera_id;
        rec.stream_id    = stream_id;
        rec.segment_path = rel;
        rec.tier         = hot_tier;
        if (!hot_pool_id.empty())
            rec.pool_id = Optional<std::string>(hot_pool_id);
        rec.status       = available;
        rec.start_time   = start_time;
        rec.end_time     = end_time;
        rec.file_size    = file_size;
        rec.created_at   = now;
        rec.updated_at   = now;

        if (!imp.exists(rec.camera_id, rec.stream_id, rec.segment_path))
            imp.upsert(rec);

        SegmentTierRange range;
        range.range_id = generateId("rng");
        range.camera_id = camera_id;
        range.stream_id = stream_id;
        range.tier = hot_tier;
        if (!hot_pool_id.empty())
            range.pool_id = Optional<std::string>(hot_pool_id);
        range.status = available;
        range.start_time = rec.start_time;
        range.end_time = rec.end_time;
        range.segment_count = 1;
        range.size_bytes = file_size;
        range.path_pattern = stream_id + "/YYYY-MM-DD/HH-MM-SS.mp4";
        range.created_at = now;
        range.updated_at = now;
        range_imp.mergeOrInsert(range);
        return true;
    }, false, true);
}

void TierStorageManager::pruneOldMetrics() {
    // Keep 30 days of metrics
    int64_t cutoff = static_cast<int64_t>(time(nullptr)) - 30LL * 86400;
    PoolMetricsImp imp;
    imp.pruneOlderThan(cutoff);

    GET_CONFIG(int64_t, segment_record_ttl_seconds, Storage::kSegmentRecordTTLSeconds);
    if (segment_record_ttl_seconds > 0) {
        SegmentTierImp seg_imp;
        seg_imp.pruneOlderThan(static_cast<int64_t>(time(nullptr)) - segment_record_ttl_seconds);
    }
}

void TierStorageManager::cleanupRestoreTempFiles() {
    GET_CONFIG(int64_t, restore_ttl_seconds, Storage::kRestoreTTLSeconds);
    if (restore_ttl_seconds < 0)
        return;

    std::string restore_root = getRestoreRootPath();
    if (restore_root.empty() || !File::is_dir(restore_root))
        return;

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int64_t cutoff = now - restore_ttl_seconds;
    RestoreJobImp restore_imp;
    SegmentTierImp seg_imp;

    int deleted_count = 0;
    int expired_job_count = 0;
    auto expired_jobs = restore_imp.queryExpiredDone(cutoff);
    for (const auto &job : expired_jobs) {
        auto segments = seg_imp.findByCamera(job.camera_id, job.start_time, job.end_time);
        for (const auto &seg : segments) {
            auto path = buildRestoreSegmentPath(seg.camera_id, seg.stream_id, seg.segment_path);
            if (::unlink(path.c_str()) == 0)
                ++deleted_count;
            std::string tmp_path = path + ".restore";
            if (::unlink(tmp_path.c_str()) == 0)
                ++deleted_count;
        }
        if (segments.empty()) {
            auto disk_segments = collectDiskSegmentsFromRoot(restore_root,
                                                             job.camera_id,
                                                             job.start_time,
                                                             job.end_time);
            for (const auto &seg : disk_segments) {
                if (::unlink(seg.full_path.c_str()) == 0)
                    ++deleted_count;
                std::string tmp_path = seg.full_path + ".restore";
                if (::unlink(tmp_path.c_str()) == 0)
                    ++deleted_count;
            }
        }

        // Mark the restore cache as expired so the same job is not processed
        // again on every cleanup tick.
        restore_imp.updateStatus(job.job_id, "EXPIRED", job.processed_bytes);
        ++expired_job_count;
    }

    // Clean up abandoned partial downloads for failed/interrupted jobs.
    File::scanDir(restore_root, [&](const std::string &path, bool isDir) {
        if (isDir || !end_with(path, ".restore"))
            return true;
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 &&
            now - static_cast<int64_t>(st.st_mtime) > restore_ttl_seconds) {
            ::unlink(path.c_str());
        }
        return true;
    }, false, true);

    if (expired_job_count > 0 || deleted_count > 0) {
        DebugL << "cleanupRestoreTempFiles: expired_jobs=" << expired_job_count
               << ", deleted_files=" << deleted_count
               << ", root=" << restore_root;
    }
}

} // namespace managerkit
