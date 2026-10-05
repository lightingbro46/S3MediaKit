/**
 * WebApiStorage.cpp
 *
 * HTTP API handlers for storage tiering (sections 1–10 of api-storage-tiering.md):
 *   1. Storage Pool APIs
 *   2. Storage Policy APIs
 *   3. Policy Assignment APIs  (camera level only; assignProject / assignGroup skipped)
 *   4. Camera Storage APIs
 *
 * Routes are mounted under /media/mserver/storage/...
 * All endpoints share the CHECK_AUTH_TOKEN() guard.
 *
 * The handler bodies declared in WebApiStorage.h are shared with the /media/api/storage/* family
 * registered in WebApi.cpp, which sits behind CHECK_SECRET() instead. Only the guards differ; both
 * families answer the same payload.
 */

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include "Manager.h"
#include "ApiWorkDispatcher.h"
#include "WebApi.h"
#include "WebApiStorage.h"
#include "WebApiErrCode.h"
#include "Camera/CameraManager.h"
#include "Common/StrUtil.h"
#include "Local/TierStorageBase.h"
#include "Local/TierStorageManager.h"
#include "Local/TierEncryptionManager.h"
#include "Local/TierEncryptionUtils.h"
#include "Storage/StoragePool.h"
#include "Storage/StoragePolicy.h"
#include "Storage/PolicyAssignment.h"
#include "Storage/TieringJob.h"
#include "Storage/StorageTierExtra.h"
#include "Local/StatisticRecorder.h"
#include "User/UserSessionCache.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

#ifdef ENABLE_TIER_STORAGE

// ===================================================================
// Shared handler bodies (see WebApiStorage.h)
//
// These run on the API worker pool and are called by both endpoint families. Where the two
// families used to answer different payloads, the `/media/mserver/storage/*` payload is the one
// kept:
//   - pool/list                    -> poolSummaryToJson (was poolDetailToJson on /media/api/...)
//   - policy/list                  -> system default policy carries the unassigned camera count
//   - policy/detail                -> policyDetailToJson without the summary fields
//   - camera/effectivePolicy/list  -> effectivePolicyToJson + the `policy_id` filter
//   - camera/timeline              -> no `policy_name` field
//   - dashboard/summary            -> capacity_complete / capacity_unavailable_pool_count
//   - policy/assignCameras         -> rejects a request whose camera_ids are all unknown
//   - pool/update                  -> watermarks are validated before the per-type validation
// ===================================================================
namespace managerkit {

namespace {

// Page size shared by every paged list of this API: an unset or out-of-range `size` falls back
// to 20, so a client cannot ask for an unbounded query.
int pagedSize(const std::string &raw) {
    int size = raw.empty() ? 20 : std::stoi(raw);
    if (size <= 0 || size > 100) size = 20;
    return size;
}

int pagedPage(const std::string &raw) {
    return raw.empty() ? 0 : std::stoi(raw);
}

int64_t optionalTime(const std::string &raw) {
    return raw.empty() ? 0 : std::stoll(raw);
}

int64_t latestTieringJobTime(const std::string &camera_id) {
    TieringJobImp imp;
    auto jobs = imp.query(camera_id, "", "", "", 0, 0, 0, 1);
    if (jobs.empty()) return 0;
    return jobs.front().updated_at > 0 ? jobs.front().updated_at : jobs.front().created_at;
}

// camera_id -> camera_name of every RTSP camera, optionally filtered by a name substring.
std::unordered_map<std::string, std::string> cameraNameMap(const std::string &search) {
    std::unordered_map<std::string, std::string> device_map;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
        auto device_tuple = src->getDeviceTuple();
        if (!search.empty() && device_tuple.name.find(search) == std::string::npos)
            return;
        device_map.emplace(device_tuple.device_id, device_tuple.name);
    }, GENERIC_RTSP_CAMERA_SCHEMA);
    return device_map;
}

Json::Value cameraStorageSummaryToJson(const std::string &camera_id, const std::string &camera_name) {
    auto summary = TierStorageManager::Instance().getCameraStorageSummary(camera_id);
    auto effective = TierStorageManager::Instance().getEffectivePolicy(camera_id);

    Json::Value data;
    data["camera_id"]           = summary.camera_id;
    data["camera_name"]         = camera_name;
    data["policy_id"]           = effective.policy_id;
    data["policy_name"]         = effective.policy_name;
    data["policy_source"]       = effective.source;
    data["total_size_bytes"]    = static_cast<Json::Int64>(summary.total_used_bytes);
    data["total_segment_count"] = static_cast<Json::Int64>(summary.total_segments);

    Json::Value tiers_arr(Json::arrayValue);
    for (const auto &t : summary.tiers) {
        Json::Value tv;
        tv["tier"]          = t.tier;
        tv["from_time"]     = static_cast<Json::Int64>(t.oldest_segment_time);
        tv["to_time"]       = static_cast<Json::Int64>(t.newest_segment_time);
        tv["size_bytes"]    = static_cast<Json::Int64>(t.used_bytes);
        tv["segment_count"] = static_cast<Json::Int64>(t.segment_count);
        tiers_arr.append(tv);
    }
    data["tier_summary"] = tiers_arr;
    data["last_tiering_job_time"] = static_cast<Json::Int64>(latestTieringJobTime(camera_id));
    data["status"] = "OK";
    return data;
}

// Splits `camera_ids` into the cameras that exist and the ids that do not.
void splitKnownCameras(const Json::Value &camera_ids_arr,
                       std::vector<std::string> &known,
                       std::vector<std::string> &unknown) {
    for (const auto &c : camera_ids_arr) {
        std::string camera_id = c.asString();
        if (findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA)) {
            known.push_back(camera_id);
        } else {
            unknown.push_back(camera_id);
        }
    }
}

// Accepts both spellings the clients use: the numeric mode (0/1/2) and its name.
bool parseEncryptionMode(const std::string &raw, RecordEncryptionMode &out) {
    if (raw.empty())
        return false;
    if (raw.find_first_not_of("0123456789") == std::string::npos) {
        int v = std::atoi(raw.c_str());
        if (v < 0 || v >= static_cast<int>(RecordEncryptionMode::kMax))
            return false;
        out = static_cast<RecordEncryptionMode>(v);
        return true;
    }
    auto mode = RecordEncryptionUtils::encryptionModeFromString(raw);
    if (mode == RecordEncryptionMode::kMax)
        return false;
    out = mode;
    return true;
}

// The encryption manager matches a finished recording to its pool by mount path, so a
// pool that gains encryption must publish its root before any file is written there.
void registerEncryptionMountPath(const StoragePool &p) {
    if (p.tier != tierTypeToString(HotTier))
        return;
    std::string root = p.mount_path.value_or("");
    if (root.empty())
        root = p.network_path.value_or("");
    if (root.empty())
        return;
    TierEncryptionManager::Instance().registerMountPath(p.id, root);
}

// Reads encryptMode from a request body. Returns false when the field is absent.
bool readEncryptionMode(const Json::Value &body, RecordEncryptionMode &out) {
    if (!body.isMember("encryptMode") || body["encryptMode"].isNull())
        return false;
    std::string raw = body["encryptMode"].isString()
                    ? body["encryptMode"].asString()
                    : std::to_string(body["encryptMode"].asInt());
    if (!parseEncryptionMode(raw, out)) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS,
                                   "Invalid encryptMode, expect 0/1/2 or none/light/strong");
    }
    return true;
}

Json::Value failedIdsToJson(const std::vector<std::string> &unknown,
                            const std::vector<std::string> &failed) {
    Json::Value failed_arr(Json::arrayValue);
    for (const auto &id : unknown) failed_arr.append(id);
    for (const auto &id : failed) failed_arr.append(id);
    return failed_arr;
}

} // namespace

// --- Section 1: storage pools -------------------------------------------------------------

static double roundMbps(float mbps) {
    return std::round(static_cast<double>(mbps) * 100.0) / 100.0;
}

// Average write/read traffic and the tier-transfer throughput as of the last tiering cycle;
// write_mbps/read_mbps stay 0 and throughput null before the first one.
static void fillPoolThroughput(const std::string &pool_id, Json::Value &item) {
    PoolThroughputStats stats;
    if (!TierStorageManager::Instance().getPoolThroughput(pool_id, stats)) {
        item["throughput"] = Json::nullValue;
        return;
    }
    item["write_mbps"] = roundMbps(stats.avg_write_mbps);
    item["read_mbps"] = roundMbps(stats.avg_read_mbps);
    item["throughput"] = stats.toJson();
}

void storagePoolList(const ArgsMap &allArgs, Json::Value &val) {
    string tier    = allArgs["tier"];
    string type    = allArgs["type"];
    string status  = allArgs["status"];
    string keyword = allArgs["keyword"];

    auto pools = TierStorageManager::Instance().listPools(tier, type, keyword);
    Json::Value data = Json::arrayValue;
    for (const auto &p : pools) {
        const std::string pool_status = p.health_status.empty() ? "OK" : p.health_status;
        if (!status.empty() && pool_status != status)
            continue;
        Json::Value item = TierStorageBase::poolSummaryToJson(p);
        fillPoolThroughput(p.id, item);
        data.append(item);
    }
    val["data"] = data;
}

void storagePoolDetail(const ArgsMap &allArgs, Json::Value &val) {
    string pool_id = allArgs["id"];
    auto pools = TierStorageManager::Instance().getPool(pool_id);
    if (pools.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
    }
    auto pool = pools.front();
    TierStorageManager::Instance().fillPoolEncryptionUsage(pool);
    val["data"] = TierStorageBase::poolDetailToJson(pool);
    fillPoolThroughput(pool.id, val["data"]);
}

void storagePoolCreate(const ArgsJson &allArgs, Json::Value &val) {
    auto pool = TierStorageBase::poolFromJson(allArgs.getArgs());

    std::string pool_err;
    if (!TierStorageBase::validatePoolByType(pool, pool_err)) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, pool_err);
    }
    if (pool.high_watermark_percent >= pool.critical_watermark_percent) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_WATERMARK_PERCENT,
                                   "high_watermark_percent must be less than critical_watermark_percent");
    }

    RecordEncryptionMode encrypt_mode = RecordEncryptionMode::kNone;
    readEncryptionMode(allArgs.getArgs(), encrypt_mode);
    const std::string encrypt_password = allArgs.getArgs().get("encryptPassword", "").asString();
    const bool encrypt_old_video = allArgs.getArgs().get("encryptOldVideo", false).asBool();

    const bool want_encryption = (encrypt_mode != RecordEncryptionMode::kNone);
    if (want_encryption) {
        if (pool.tier != tierTypeToString(HotTier)) {
            throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS,
                                       "Encryption is only supported on HOT tier pools");
        }
        if (encrypt_password.empty()) {
            throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS,
                                       "encryptPassword is required when encryptMode is not none");
        }
    }

    auto id = TierStorageManager::Instance().createPool(pool);
    if (id.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Failed to create storage pool");
    }
    val["data"]["id"] = id;

    if (!want_encryption)
        return;

    pool.id = id;
    registerEncryptionMountPath(pool);
    auto &enc_mgr = TierEncryptionManager::Instance();
    if (!enc_mgr.initialConfigTier(id, encrypt_mode, encrypt_password, encrypt_old_video)) {
        // A pool whose encryption never came up would record in plaintext while the
        // client believes it is encrypted, so the creation is rolled back instead.
        int ref = 0;
        bool is_default = false;
        TierStorageManager::Instance().deletePool(id, ref, is_default);
        enc_mgr.unregisterMountPath(id);
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                   "Failed to initialize encryption config, pool creation rolled back");
    }

    if (encrypt_old_video) {
        std::string job_err;
        bool started = enc_mgr.startReencryptJob(id, &job_err);
        val["data"]["reencrypt_started"] = started;
        if (!started) {
            val["data"]["reencrypt_error"] = job_err;
            WarnL << "Pool " << id << " created, but the re-encryption job for existing data "
                     "could not be started: " << job_err;
        }
    }

    auto created = TierStorageManager::Instance().getPool(id);
    if (!created.empty())
        val["data"]["encryption"] = TierStorageBase::encryptionStatusToJson(created.front());
}

void storagePoolUpdate(const ArgsJson &allArgs, Json::Value &val) {
    string pool_id = allArgs["id"];

    auto existing = TierStorageManager::Instance().getPool(pool_id);
    if (existing.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
    }
    auto body = allArgs.getArgs();

    auto &base = existing.front();
    TierStorageBase::applyPoolPatch(base, body);

    if (base.high_watermark_percent >= base.critical_watermark_percent) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_WATERMARK_PERCENT,
                                   "high_watermark_percent must be less than critical_watermark_percent");
    }
    std::string pool_err;
    if (!TierStorageBase::validatePoolByType(base, pool_err)) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, pool_err);
    }

    RecordEncryptionMode encrypt_mode = RecordEncryptionMode::kNone;
    const bool has_encrypt_mode = readEncryptionMode(body, encrypt_mode);
    const bool has_encrypt_old = body.isMember("encryptOldVideo") && !body["encryptOldVideo"].isNull();
    const bool encrypt_old_video = has_encrypt_old && body["encryptOldVideo"].asBool();
    const std::string encrypt_password = body.get("encryptPassword", "").asString();

    auto &enc_mgr = TierEncryptionManager::Instance();
    const bool is_hot = (base.tier == tierTypeToString(HotTier));
    const bool configured = enc_mgr.tierExists(pool_id);

    RecordEncryptionMode current_mode = RecordEncryptionMode::kNone;
    bool current_eov = false;
    std::string verify_salt, verify_hash;
    if (configured) {
        auto cfg = enc_mgr.getTierConfig(pool_id);
        current_mode = cfg.mode;
        current_eov  = cfg.encrypt_old_video;
        verify_salt  = cfg.verify_salt;
        verify_hash  = cfg.verify_hash;
    }

    const bool mode_changed = has_encrypt_mode && (encrypt_mode != current_mode);
    // encryptOldVideo only means something once a mode is, or is becoming, non-none.
    const bool eov_applicable = configured || (mode_changed && encrypt_mode != RecordEncryptionMode::kNone);
    const bool eov_changed = has_encrypt_old && eov_applicable && (encrypt_old_video != current_eov);
    const bool enc_changed = mode_changed || eov_changed;

    if (enc_changed && !is_hot) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS,
                                   "Encryption is only supported on HOT tier pools");
    }
    if (enc_changed && enc_mgr.isAnyJobRunning()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_ENCRYPTION_REENCRYPT_JOB_RUNNING,
                                   "A re-encrypt job is running for tier: " + enc_mgr.getRunningJobTierId());
    }
    if (mode_changed) {
        if (encrypt_password.empty()) {
            throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, configured
                                       ? "encryptPassword is required to change encryptMode"
                                       : "encryptPassword is required to enable encryption");
        }
        if (configured && !RecordEncryptionUtils::verifyPassword(encrypt_password, verify_salt, verify_hash)) {
            throw ApiResponseException(ApiErrCode::CODE_STORAGE_ENCRYPTION_INVALID_OLD_PASSWORD,
                                       "Invalid encryptPassword");
        }
    }

    if (!TierStorageManager::Instance().updatePool(base)) {
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Failed to update storage pool");
    }
    val["data"]["id"] = pool_id;

    registerEncryptionMountPath(base);
    if (mode_changed && !configured) {
        if (!enc_mgr.initialConfigTier(pool_id, encrypt_mode, encrypt_password, encrypt_old_video)) {
            throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                       "Failed to initialize encryption config");
        }
        if (encrypt_old_video) {
            std::string job_err;
            val["data"]["reencrypt_started"] = enc_mgr.startReencryptJob(pool_id, &job_err);
            if (!job_err.empty())
                val["data"]["reencrypt_error"] = job_err;
        }
    } else {
        if (eov_changed) {
            std::string err;
            // A mode change right after this starts its own pass, and only one job may
            // run per tier, so the option must not trigger a second one.
            if (!enc_mgr.updateEncryptOldVideoOption(pool_id, encrypt_old_video,
                                                     /*trigger_reencrypt=*/!mode_changed, &err)) {
                throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                           err.empty() ? "Failed to update encryptOldVideo" : err);
            }
        }
        if (mode_changed) {
            std::string err;
            if (!enc_mgr.updateEncryptionMode(pool_id, encrypt_mode, encrypt_password,
                                              /*trigger_reencrypt=*/true, &err)) {
                throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                           err.empty() ? "Failed to change encryptMode" : err);
            }
        }
    }

    auto refreshed = TierStorageManager::Instance().getPool(pool_id);
    if (!refreshed.empty())
        val["data"]["encryption"] = TierStorageBase::encryptionStatusToJson(refreshed.front());
}

void storagePoolDelete(const ArgsMap &allArgs, Json::Value &val) {
    string pool_id = allArgs["id"];
    auto &enc_mgr = TierEncryptionManager::Instance();

    if (TierStorageManager::Instance().getPool(pool_id).empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
    }

    if (enc_mgr.tierExists(pool_id)) {
        if (enc_mgr.isAnyJobRunning() && enc_mgr.getRunningJobTierId() == pool_id) {
            throw ApiResponseException(ApiErrCode::CODE_STORAGE_ENCRYPTION_REENCRYPT_JOB_RUNNING,
                                       "Cannot delete pool while its re-encrypt job is running");
        }
        if (enc_mgr.hasEncryptedData(pool_id)) {
            // Deleting the pool destroys the key, so any segment still keyed to it would
            // become unreadable wherever it now sits.
            int origin_segments = TierStorageManager::Instance().countSegmentsByOriginPool(pool_id);
            if (origin_segments < 0) {
                throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_IN_USE,
                                           "Cannot verify whether encrypted segments still depend on this pool, retry later");
            }
            if (origin_segments > 0) {
                val["data"]["origin_segment_count"] = origin_segments;
                throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_IN_USE,
                                           "Cannot delete pool: " + std::to_string(origin_segments) +
                                           " encrypted segment(s) still depend on its encryption key. "
                                           "Enable encryptOldVideo and set encryption mode to none to "
                                           "decrypt them first.");
            }
        }
    }

    int ref_count = 0;
    bool is_default_pool = false;
    if (!TierStorageManager::Instance().deletePool(pool_id, ref_count, is_default_pool)) {
        val["data"]["ref_count"] = ref_count;
        val["data"]["is_default_pool"] = is_default_pool;
        if (is_default_pool) {
            throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_DEFAULT_CANNOT_DELETE,
                                       "Cannot delete system default storage pool");
        }
        if (ref_count > 0) {
            throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_IN_USE,
                                       "Storage pool is referenced by " + std::to_string(ref_count) +
                                       " policies or segment ranges");
        }
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Failed to delete storage pool");
    }

    if (enc_mgr.tierExists(pool_id) && !enc_mgr.removeTierConfig(pool_id)) {
        WarnL << "Pool " << pool_id << " deleted but its encryption config could not be removed";
    }
    // Otherwise the dead tier id keeps shadowing the mount for the next pool on it.
    enc_mgr.unregisterMountPath(pool_id);
}

void storagePoolEncryptChangePassword(const ArgsMap &allArgs, Json::Value &val) {
    string pool_id      = allArgs["id"];
    string old_password = allArgs["old_password"];
    string new_password = allArgs["new_password"];

    if (new_password.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, "new_password must not be empty");
    }
    if (new_password == old_password) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_ENCRYPTION_PASSWORD_SAME,
                                   "new_password must be different from old_password");
    }

    auto pools = TierStorageManager::Instance().getPool(pool_id);
    if (pools.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
    }

    auto &enc_mgr = TierEncryptionManager::Instance();
    if (!enc_mgr.tierExists(pool_id)) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS,
                                   "Encryption has not been configured for this pool");
    }
    if (enc_mgr.isAnyJobRunning()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_ENCRYPTION_REENCRYPT_JOB_RUNNING,
                                   "A re-encrypt job is running for tier: " + enc_mgr.getRunningJobTierId());
    }

    auto cfg = enc_mgr.getTierConfig(pool_id);
    if (!RecordEncryptionUtils::verifyPassword(old_password, cfg.verify_salt, cfg.verify_hash)) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_ENCRYPTION_INVALID_OLD_PASSWORD, "Invalid old_password");
    }

    std::string err;
    // trigger_reencrypt=false: the new key applies to new recordings, and existing
    // footage is rewritten by the pass the manager schedules for the encrypted scope.
    if (!enc_mgr.updateEncryptionSalts(pool_id, old_password, new_password,
                                       /*trigger_reencrypt=*/false, &err)) {
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                   err.empty() ? "Failed to change encryption password" : err);
    }

    val["data"]["id"] = pool_id;
    val["data"]["encryption"] = TierStorageBase::encryptionStatusToJson(pools.front());
}

void storagePoolTestConnection(const ArgsJson &allArgs, Json::Value &val) {
    const Json::Value &body = allArgs.getArgs();
    string pool_id = body.get("pool_id", "").asString();
    StoragePool pool;
    if (!pool_id.empty()) {
        // Test an existing saved pool
        auto existing = TierStorageManager::Instance().getPool(pool_id);
        if (existing.empty()) {
            throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
        }
        pool = existing.front();
        TierStorageBase::applyPoolPatch(pool, body);
    } else {
        // Ad-hoc test (before pool is saved)
        pool = TierStorageBase::poolFromJson(body);
    }

    std::string validation_error;
    if (!TierStorageBase::validatePoolByType(pool, validation_error)) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, validation_error);
    }

    string message;
    int latency_ms = 0;
    bool ok = TierStorageManager::Instance().testPoolConnection(pool, message, latency_ms);

    Json::Value data;
    data["status"]     = ok ? "OK" : "ERROR";
    data["latency_ms"] = latency_ms;
    data["can_read"]   = ok;
    data["can_write"]  = ok;
    data["message"]    = message;
    val["data"] = data;
    if (!ok) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_CONN_FAILED, message);
    }
}

void storagePoolOptions(Json::Value &val) {
    Json::Value data;
    data["poolsTypeSupport"] = Json::objectValue;
    data["poolsTypeSupport"]["HOT"]  = poolTypeSupport(TierType::HotTier);
    data["poolsTypeSupport"]["WARM"] = poolTypeSupport(TierType::WarmTier);
    data["poolsTypeSupport"]["COLD"] = poolTypeSupport(TierType::ColdTier);
    data["encryptionSupport"] = Json::objectValue;
    data["encryptionSupport"]["HOT"]  = poolEncryptionSupport(TierType::HotTier);
    data["encryptionSupport"]["WARM"] = poolEncryptionSupport(TierType::WarmTier);
    data["encryptionSupport"]["COLD"] = poolEncryptionSupport(TierType::ColdTier);
    val["data"] = data;
}

// --- Section 2: storage policies ------------------------------------------------------------

void storagePolicyList(const ArgsMap &allArgs, Json::Value &val) {
    string keyword = allArgs["keyword"];
    int enabled_filter = !allArgs["enabled"].empty() ? allArgs["enabled"].as<int>() : -1;
    int page = pagedPage(allArgs["page"]);
    int size = pagedSize(allArgs["size"]);

    auto policies = TierStorageManager::Instance().listPolicies(keyword, enabled_filter, page, size);
    auto total_count = TierStorageManager::Instance().countPolicies(keyword, enabled_filter);

    Json::Value items = Json::arrayValue;
    PolicyAssignmentImp assignment_imp;
    int total_device_count = CameraManager::Instance().getCameraKeys().size();
    int total_applied_count = 0;
    auto system_default_policy_id = TierStorageManager::Instance().getSystemDefaultPolicyId();
    for (const auto &p : policies) {
        if (p.id == system_default_policy_id) continue; // skip system default policy for now
        const int applied_count = static_cast<int>(assignment_imp.findCamerasByPolicyId(p.id).size());
        total_applied_count += applied_count;
        items.append(TierStorageBase::policySummaryToJson(p, applied_count));
    }
    // Add the system default policy with the remaining camera count
    for (const auto &p : policies) {
        if (p.id == system_default_policy_id) {
            items.append(TierStorageBase::policySummaryToJson(p, total_device_count - total_applied_count));
            break;
        }
    }

    Json::Value data;
    data["items"] = items;
    data["page"]  = page;
    data["size"]  = size;
    data["total"] = total_count;
    val["data"]   = data;
}

void storagePolicyDetail(const ArgsMap &allArgs, Json::Value &val) {
    string policy_id = allArgs["id"];

    auto policies = TierStorageManager::Instance().getPolicy(policy_id);
    if (policies.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
    }

    val["data"] = TierStorageBase::policyDetailToJson(
        policies.front(), TierStorageBase::loadPoolMap());
}

void storagePolicyCreate(const ArgsJson &allArgs, Json::Value &val) {
    auto policy = TierStorageBase::policyFromJson(allArgs.getArgs());

    std::string tier_err;
    if (!TierStorageBase::validateStoragePolicy(policy, TierStorageBase::loadPoolMap(), tier_err)) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, tier_err);
    }

    auto id = TierStorageManager::Instance().createPolicy(policy);
    if (id.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Failed to create storage policy");
    }
    val["data"]["id"] = id;
}

void storagePolicyUpdate(const ArgsJson &allArgs, Json::Value &val) {
    string policy_id = allArgs["id"];
    auto existing = TierStorageManager::Instance().getPolicy(policy_id);
    if (existing.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
    }

    auto policy = TierStorageBase::policyFromJson(allArgs.getArgs());

    std::string tier_err;
    if (!TierStorageBase::validateStoragePolicy(policy, TierStorageBase::loadPoolMap(), tier_err)) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, tier_err);
    }

    if (!TierStorageManager::Instance().updatePolicy(policy)) {
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Failed to update storage policy");
    }
    val["data"]["id"] = policy_id;
}

void storagePolicyDelete(const ArgsMap &allArgs, Json::Value &val) {
    string policy_id = allArgs["id"];
    auto existing = TierStorageManager::Instance().getPolicy(policy_id);
    if (existing.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
    }

    int camera_count = 0;
    bool is_default_policy = false;
    if (!TierStorageManager::Instance().deletePolicy(policy_id, camera_count, is_default_policy)) {
        val["data"]["camera_count"] = camera_count;
        val["data"]["is_default_policy"] = is_default_policy;
        if (is_default_policy) {
            throw ApiResponseException(ApiErrCode::CODE_STORAGE_POLICY_DEFAULT_CANNOT_DELETE,
                                       "Cannot delete system default storage policy");
        }
        if (camera_count > 0) {
            throw ApiResponseException(ApiErrCode::CODE_STORAGE_POLICY_IN_USE,
                                       "Policy is applied to " + std::to_string(camera_count) + " cameras");
        }
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                   "Policy not found or could not be deleted");
    }
}

// --- Section 3: policy assignment (camera level) ---------------------------------------------

void storagePolicyAssignCamera(const ArgsMap &allArgs, Json::Value &val) {
    string camera_id = allArgs["camera_id"];
    string policy_id = allArgs["policy_id"];
    string reason = allArgs["override_reason"];

    auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
    if (!device) {
        throw ApiResponseException(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
    }

    auto existing = TierStorageManager::Instance().getPolicy(policy_id);
    if (existing.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
    }

    if (!TierStorageManager::Instance().assignPolicyToCamera(camera_id, policy_id, reason)) {
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                   "Assign policy to camera failed");
    }
}

void storagePolicyAssignCameras(const ArgsJson &allArgs, Json::Value &val) {
    std::string policy_id = allArgs["policy_id"];
    Json::Value camera_ids_arr = allArgs.getArgs()["camera_ids"];
    if (camera_ids_arr.empty() || !camera_ids_arr.isArray()) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, "camera_ids must be an array");
    }

    std::vector<std::string> camera_ids;
    std::vector<std::string> non_existed_camera_ids;
    splitKnownCameras(camera_ids_arr, camera_ids, non_existed_camera_ids);

    if (camera_ids.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, "No valid camera IDs provided");
    }

    auto existing = TierStorageManager::Instance().getPolicy(policy_id);
    if (existing.empty()) {
        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
    }

    std::vector<std::string> failed;
    int assigned = TierStorageManager::Instance().assignPolicyToCameras(camera_ids, policy_id, failed);

    Json::Value data;
    data["assigned_count"] = assigned;
    data["failed_ids"] = failedIdsToJson(non_existed_camera_ids, failed);
    val["data"] = data;
}

void storagePolicyRemoveCamera(const ArgsMap &allArgs, Json::Value &val) {
    string camera_id = allArgs["camera_id"];
    auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
    if (!device) {
        throw ApiResponseException(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
    }
    if (!TierStorageManager::Instance().removeCameraOverride(camera_id)) {
        throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                   "Failed to unassign storage policy from camera");
    }
}

void storagePolicyRemoveCameras(const ArgsJson &allArgs, Json::Value &val) {
    Json::Value camera_ids_arr = allArgs.getArgs()["camera_ids"];
    if (camera_ids_arr.empty() || !camera_ids_arr.isArray()) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, "camera_ids must be an array");
    }

    std::vector<std::string> camera_ids;
    std::vector<std::string> non_existed_camera_ids;
    splitKnownCameras(camera_ids_arr, camera_ids, non_existed_camera_ids);

    std::vector<std::string> failed;
    int removed = TierStorageManager::Instance().removeCamerasOverride(camera_ids, failed);

    Json::Value data;
    data["removed_count"] = removed;
    data["failed_ids"] = failedIdsToJson(non_existed_camera_ids, failed);
    val["data"] = data;
}

// --- Section 4: camera storage ----------------------------------------------------------------

void storageCameraEffectivePolicyList(const ArgsMap &allArgs, Json::Value &val) {
    string search = allArgs["search"];
    string policy_id = allArgs["policy_id"];

    auto device_map = cameraNameMap(search);

    Json::Value items(Json::arrayValue);
    for (const auto &it : device_map) {
        auto result = TierStorageManager::Instance().getEffectivePolicy(it.first);
        if (!policy_id.empty() && result.policy_id != policy_id)
            continue;
        items.append(TierStorageBase::effectivePolicyToJson(result, it.second));
    }

    Json::Value data;
    data["total"] = static_cast<int>(items.size());
    data["items"] = items;
    val["data"] = data;
}

void storageCameraTimeline(const ArgsMap &allArgs, Json::Value &val) {
    string camera_id     = allArgs["camera_id"];
    int64_t start_time   = allArgs["start_time"];
    int64_t end_time     = allArgs["end_time"];
    bool include_deleted = allArgs["include_deleted"];

    if (start_time <= 0 || end_time <= 0 || start_time >= end_time) {
        throw ApiResponseException(ApiErrCode::CODE_INVALID_TIME_RANGE, "start_time and end_time must be valid");
    }

    auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
    if (!device) {
        throw ApiResponseException(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
    }
    auto device_tuple = device->getDeviceTuple();

    Json::Value ranges_json(Json::arrayValue);
    auto pool_map = TierStorageBase::loadPoolMap();
    auto ranges = TierStorageManager::Instance().getCameraTimeline(camera_id, start_time, end_time);
    for (const auto &r : ranges) {
        if (!include_deleted && r.status == segmentStatusToString(SegmentStatus::DELETED))
            continue;
        Json::Value range;
        range["start"]            = static_cast<Json::Int64>(r.start);
        range["end"]              = static_cast<Json::Int64>(r.end);
        range["tier"]             = r.tier;
        range["pool_id"]          = r.pool_id;
        range["status"]           = r.status;
        range["segment_count"]    = static_cast<Json::Int64>(r.segment_count);
        range["size_bytes"]       = static_cast<Json::Int64>(r.size_bytes);
        range["restore_required"] = TierStorageBase::isRestoreRequiredPool(r.pool_id, pool_map);
        range["has_motion"]       = r.has_motion;
        range["has_event"]        = r.has_event;
        ranges_json.append(range);
    }

    auto effective = TierStorageManager::Instance().getEffectivePolicy(camera_id);
    Json::Value data;
    data["camera_id"]   = camera_id;
    data["camera_name"] = device_tuple.name;
    data["policy_id"]   = effective.policy_id;
    data["ranges"]      = ranges_json;
    val["data"] = data;
}

void storageCameraSummary(const ArgsMap &allArgs, Json::Value &val) {
    string camera_id = allArgs["camera_id"];
    auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
    if (!device) {
        throw ApiResponseException(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
    }
    val["data"] = cameraStorageSummaryToJson(camera_id, device->getDeviceTuple().name);
}

// --- Sections 5-6: restore / tiering jobs -----------------------------------------------------

void storageRestoreJobList(const ArgsMap &allArgs, Json::Value &val) {
    std::string camera_id = allArgs["camera_id"];
    std::string status = allArgs["status"];
    int64_t from_time = allArgs["from_time"];
    int64_t to_time = allArgs["to_time"];
    int page = pagedPage(allArgs["page"]);
    int size = pagedSize(allArgs["size"]);

    auto jobs = TierStorageManager::Instance().listRestoreJobs(camera_id, status, from_time, to_time, page, size);
    Json::Value items(Json::arrayValue);
    for (const auto &j : jobs) {
        auto device = findDeviceSource(j.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        items.append(TierStorageBase::restoreJobToJson(
            j, device ? device->getDeviceTuple().name : "", true));
    }

    Json::Value data;
    data["items"] = items;
    data["page"] = page;
    data["size"] = size;
    data["total"] = TierStorageManager::Instance().countRestoreJobs(camera_id, status, from_time, to_time);
    val["data"] = data;
}

void storageTieringJobList(const ArgsMap &allArgs, Json::Value &val) {
    std::string status = allArgs["status"];
    std::string camera_id = allArgs["camera_id"];
    std::string source_tier = allArgs["source_tier"];
    std::string target_tier = allArgs["target_tier"];
    int64_t from_time = allArgs["from_time"];
    int64_t to_time = allArgs["to_time"];
    int page = pagedPage(allArgs["page"]);
    int size = pagedSize(allArgs["size"]);

    Json::Value items(Json::arrayValue);
    auto jobs = TierStorageManager::Instance().listTieringJobs(
        camera_id, status, source_tier, target_tier, from_time, to_time, page, size);
    for (const auto &j : jobs) {
        auto device = findDeviceSource(j.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        items.append(TierStorageBase::tieringJobToJson(
            j, device ? device->getDeviceTuple().name : ""));
    }

    Json::Value data;
    data["items"] = items;
    data["page"] = page;
    data["size"] = size;
    data["total"] = TierStorageManager::Instance().countTieringJobs(camera_id, status, from_time, to_time);
    val["data"] = data;
}

// --- Section 7: dashboard ---------------------------------------------------------------------

void storageDashboardSummary(Json::Value &val) {
    auto pools = TierStorageManager::Instance().listPools();

    struct TierAgg {
        int64_t total = 0;
        int64_t used = 0;
        float write_mbps = 0.0f;
        float read_mbps = 0.0f;
        float record_ingest_mbps = 0.0f;
        // HOT only: the tightest pool, -1 when no pool has a forecast yet.
        double remaining_days = -1;
        double retention_days = -1;
        bool retention_low = false;
        std::string status = "OK";
    };
    struct TierAggCompare {
        bool operator()(const std::string &x, const std::string &y) const {
            return tierTypeFromString(x) < tierTypeFromString(y);
        }
    };
    std::map<std::string, TierAgg, TierAggCompare> tiers;
    tiers["HOT"];
    tiers["WARM"];
    tiers["COLD"];

    int64_t total_bytes = 0;
    int64_t used_bytes = 0;
    int capacity_unavailable_pool_count = 0;
    std::string status = "OK";
    Json::Value alerts(Json::arrayValue);

    for (const auto &p : pools) {
        const std::string pool_status = p.health_status.empty() ? "OK" : p.health_status;
        auto &agg = tiers[p.tier];
        agg.total += p.total_bytes;
        agg.used += p.used_bytes;
        agg.status = TierStorageBase::worstStorageStatus(agg.status, pool_status);
        PoolThroughputStats throughput;
        if (TierStorageManager::Instance().getPoolThroughput(p.id, throughput)) {
            agg.write_mbps += throughput.avg_write_mbps;
            agg.read_mbps += throughput.avg_read_mbps;
        }
        agg.record_ingest_mbps += TierStorageManager::Instance().getPoolIngest(p.id).mbps;
        RetentionForecast forecast;
        if (p.enabled && TierStorageManager::Instance().getPoolRetentionForecast(p.id, forecast) &&
            forecast.retention_days_estimate >= 0) {
            auto tighter = [](double cur, double v) { return cur < 0 || v < cur ? v : cur; };
            agg.remaining_days = tighter(agg.remaining_days, forecast.estimated_remaining_days);
            agg.retention_days = tighter(agg.retention_days, forecast.retention_days_estimate);
            agg.retention_low = agg.retention_low || forecast.level > 0;
        }
        total_bytes += p.total_bytes;
        used_bytes += p.used_bytes;
        if (p.enabled && !p.capacity_available)
            ++capacity_unavailable_pool_count;
        status = TierStorageBase::worstStorageStatus(status, pool_status);

        const int used_percent = p.total_bytes > 0
            ? static_cast<int>((p.used_bytes * 100) / p.total_bytes)
            : static_cast<int>(p.usage_pct);
        if (pool_status != "OK" || used_percent >= p.high_watermark_percent) {
            Json::Value alert;
            alert["level"] = pool_status == "OK" ? "WARNING" : pool_status;
            alert["message"] = p.name + " used percent is " + std::to_string(used_percent) + "%";
            alerts.append(alert);
        }
    }

    Json::Value tier_summary(Json::arrayValue);
    for (const auto &kv : tiers) {
        const auto &agg = kv.second;
        Json::Value t;
        t["tier"] = kv.first;
        t["used_percent"] = agg.total > 0 ? static_cast<int>((agg.used * 100) / agg.total) : 0;
        t["status"] = agg.status;
        t["total_bytes"] = static_cast<Json::Int64>(agg.total);
        t["used_bytes"] = static_cast<Json::Int64>(agg.used);
        // Forecast for the HOT tier only; 0 (and retention_days_estimate null) when unknown.
        t["estimated_remaining_days"] = agg.remaining_days >= 0 ? std::round(agg.remaining_days * 100.0) / 100.0 : 0.0;
        t["retention_days_estimate"] = agg.retention_days >= 0 ? Json::Value(std::round(agg.retention_days * 100.0) / 100.0)
                                                               : Json::Value(Json::nullValue);
        t["retention_state"] = agg.retention_low ? "low" : "normal";
        t["write_mbps"] = roundMbps(agg.write_mbps);
        t["read_mbps"] = roundMbps(agg.read_mbps);
        t["record_ingest_mbps"] = roundMbps(agg.record_ingest_mbps);
        tier_summary.append(t);
    }

    TieringJobImp tiering_imp;
    RestoreJobImp restore_imp;

    Json::Value data;
    data["total_bytes"] = static_cast<Json::Int64>(total_bytes);
    data["used_bytes"] = static_cast<Json::Int64>(used_bytes);
    data["free_bytes"] = static_cast<Json::Int64>(total_bytes > used_bytes ? total_bytes - used_bytes : 0);
    data["used_percent"] = total_bytes > 0 ? static_cast<int>((used_bytes * 100) / total_bytes) : 0;
    data["status"] = status;
    data["capacity_complete"] = capacity_unavailable_pool_count == 0;
    data["capacity_unavailable_pool_count"] = capacity_unavailable_pool_count;
    data["tier_summary"] = tier_summary;
    data["active_tiering_jobs"] = tiering_imp.countQuery("", "PENDING") + tiering_imp.countQuery("", "RUNNING");
    data["failed_tiering_jobs"] = tiering_imp.countQuery("", "FAILED");
    data["active_restore_jobs"] = restore_imp.countActive();
    data["alerts"] = alerts;
    val["data"] = data;
}

void storageDashboardDetail(Json::Value &val) {
    val["data"] = TierStorageManager::Instance().getDashboardDetail();
}

// --- Sections 8-10: alerts, expired segments, protected videos --------------------------------

void storageAlertList(const ArgsMap &allArgs, Json::Value &val) {
    std::string level = allArgs["level"];
    int acknowledged = allArgs["acknowledged"].empty() ? -1 : std::stoi(allArgs["acknowledged"]);
    int page = pagedPage(allArgs["page"]);
    int size = pagedSize(allArgs["size"]);

    StorageAlertImp imp;
    Json::Value items(Json::arrayValue);
    auto pool_map = TierStorageBase::loadPoolMap();
    for (const auto &a : imp.query(level, acknowledged, page, size))
        items.append(TierStorageBase::alertToJson(a, pool_map));

    Json::Value data;
    data["items"] = items;
    data["page"] = page;
    data["size"] = size;
    data["total"] = imp.countQuery(level, acknowledged);
    val["data"] = data;
}

void storageExpiredSegmentList(const ArgsMap &allArgs, Json::Value &val) {
    std::string camera_id = allArgs["camera_id"];
    int64_t from_time = optionalTime(allArgs["from_time"]);
    int64_t to_time = optionalTime(allArgs["to_time"]);
    int page = pagedPage(allArgs["page"]);
    int size = pagedSize(allArgs["size"]);

    SegmentTierRangeImp imp;
    Json::Value items(Json::arrayValue);
    for (const auto &r : imp.findExpired(camera_id, from_time, to_time, page, size)) {
        auto device = findDeviceSource(r.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        ProtectedVideoImp protected_imp;
        items.append(TierStorageBase::expiredSegmentToJson(
            r,
            device ? device->getDeviceTuple().name : "",
            protected_imp.overlaps(r.camera_id, r.start_time, r.end_time)));
    }

    Json::Value data;
    data["items"] = items;
    data["page"] = page;
    data["size"] = size;
    data["total"] = imp.countExpired(camera_id, from_time, to_time);
    val["data"] = data;
}

void storageProtectedList(const ArgsMap &allArgs, Json::Value &val) {
    std::string camera_id = allArgs["camera_id"];
    std::string type = allArgs["type"];
    int64_t from_time = optionalTime(allArgs["from_time"]);
    int64_t to_time = optionalTime(allArgs["to_time"]);
    int page = pagedPage(allArgs["page"]);
    int size = pagedSize(allArgs["size"]);

    ProtectedVideoImp imp;
    Json::Value items(Json::arrayValue);
    for (const auto &p : imp.query(camera_id, type, from_time, to_time, page, size)) {
        auto device = findDeviceSource(p.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        items.append(TierStorageBase::protectedVideoToJson(
            p, device ? device->getDeviceTuple().name : ""));
    }

    Json::Value data;
    data["items"] = items;
    data["page"] = page;
    data["size"] = size;
    data["total"] = imp.countQuery(camera_id, type, from_time, to_time);
    val["data"] = data;
}

// --- Mount points -----------------------------------------------------------------------------

void storageMountpointAvailable(const ArgsMap &allArgs, Json::Value &val) {
    string storage_types = allArgs["include"];
    auto available_mount_points = TierStorageManager::Instance().getAvailableMountPoints(storage_types);

    Json::Value data;
    data["mount_point"] = Json::arrayValue;
    for (const auto &mp : available_mount_points) {
        data["mount_point"].append(mp.toJson());
    }
    val["data"] = data;
}

} // namespace managerkit

// ===================================================================
// Register all storage API routes
//
// Each route keeps its own guards (CHECK_AUTH_TOKEN + CHECK_USER_PERMISSION here, CHECK_SECRET in
// the /media/api/storage/* family) and dispatches the shared body to the API worker pool.
// ===================================================================
namespace managerkit {

void registerStorageApis() {

    // ================================================================
    // === Section 1: Storage Pool APIs ================================
    // ================================================================

    // GET/POST /media/mserver/storage/pool/list
    api_regist("/media/mserver/storage/pool/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePoolList(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // GET/POST /media/mserver/storage/pool/detail
    api_regist("/media/mserver/storage/pool/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePoolDetail(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // POST /media/mserver/storage/pool/create
    api_regist("/media/mserver/storage/pool/create", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("name", "type", "tier");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePoolCreate(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/pool/update
    api_regist("/media/mserver/storage/pool/update", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePoolUpdate(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/pool/delete
    api_regist("/media/mserver/storage/pool/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePoolDelete(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/pool/encrypt/changePassword
    api_regist("/media/mserver/storage/pool/encrypt/changePassword", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id", "old_password", "new_password");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePoolEncryptChangePassword(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/pool/testConnection
    api_regist("/media/mserver/storage/pool/testConnection", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);

        // The connection test (filesystem/S3 round trip) runs on the API worker, but not in the
        // serial mutation group: it writes nothing. No read group either, because a pool that does
        // not answer holds its worker for the whole connect timeout.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePoolTestConnection(allArgs, val);
        });
    });

    // ================================================================
    // === Section 2: Storage Policy APIs ==============================
    // ================================================================

    // GET/POST /media/mserver/storage/policy/list
    api_regist("/media/mserver/storage/policy/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyList(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // GET/POST /media/mserver/storage/policy/detail
    api_regist("/media/mserver/storage/policy/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyDetail(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // POST /media/mserver/storage/policy/create
    api_regist("/media/mserver/storage/policy/create", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("name", "total_retention_days");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyCreate(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/policy/update
    api_regist("/media/mserver/storage/policy/update", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyUpdate(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    api_regist("/media/mserver/storage/policy/clone", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id", "name");

        // Mutations run on the API worker, one at a time: they update the DB and the shared pool/policy caches and registrations.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            string policy_id = allArgs["id"];
            string new_name  = allArgs["name"];

            auto existing = TierStorageManager::Instance().getPolicy(policy_id);
            if (existing.empty()) {
                throw ApiResponseException(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            }

            auto new_id = TierStorageManager::Instance().clonePolicy(policy_id, new_name);
            if (new_id.empty()) {
                throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Clone storage policy failed");
            }

            val["data"]["id"] = new_id;
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/policy/delete
    api_regist("/media/mserver/storage/policy/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyDelete(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // ================================================================
    // === Section 3: Policy Assignment APIs (camera level) ============
    // ================================================================

    // POST /media/mserver/storage/policy/assignCamera
    api_regist("/media/mserver/storage/policy/assignCamera", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id", "policy_id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyAssignCamera(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/policy/assignCameras
    api_regist("/media/mserver/storage/policy/assignCameras", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("policy_id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyAssignCameras(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/policy/removeCamera
    api_regist("/media/mserver/storage/policy/removeCamera", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyRemoveCamera(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // POST /media/mserver/storage/policy/removeCameras
    api_regist("/media/mserver/storage/policy/removeCameras", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storagePolicyRemoveCameras(allArgs, val);
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    api_regist("/media/mserver/storage/camera/effectivePolicy", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id");

        // Repository queries and JSON building run on the API worker.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            string camera_id = allArgs["camera_id"];
            auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!device) {
                throw ApiResponseException(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            }
            auto device_tuple = device->getDeviceTuple();
            auto result = TierStorageManager::Instance().getEffectivePolicy(device_tuple.device_id);
            val["data"] = TierStorageBase::effectivePolicyToJson(result, device_tuple.name);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // GET/POST /media/mserver/storage/camera/effectivePolicy/list
    api_regist("/media/mserver/storage/camera/effectivePolicy/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageCameraEffectivePolicyList(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // ================================================================
    // === Section 4: Camera Storage APIs ==============================
    // ================================================================

    // GET/POST /media/mserver/storage/camera/timeline
    api_regist("/media/mserver/storage/camera/timeline", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id", "start_time", "end_time");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageCameraTimeline(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // GET/POST /media/mserver/storage/camera/summary
    api_regist("/media/mserver/storage/camera/summary", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id");

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageCameraSummary(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    api_regist("/media/mserver/storage/camera/summary/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        // Repository queries and JSON building run on the API worker. One item per camera, each
        // built exactly like /media/mserver/storage/camera/summary.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            Json::Value items(Json::arrayValue);
            for (const auto &it : cameraNameMap(allArgs["search"])) {
                items.append(cameraStorageSummaryToJson(it.first, it.second));
            }
            val["data"] = items;
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // ================================================================
    // === Section 5: Playback APIs ===================================
    // ================================================================

    api_regist("/media/mserver/storage/playback/resolve", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("camera_id", "start_time", "end_time");

        string camera_id = allArgs["camera_id"];
        string stream_id = allArgs["stream_id"];
        int64_t start_time = allArgs["start_time"];
        int64_t end_time = allArgs["end_time"];
        string protocol = allArgs["protocol"];
        if (protocol.empty()) {
            protocol = "HTTP_MP4";
        }

        if (start_time <= 0 || end_time <= 0 || start_time >= end_time) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_TIME_RANGE, "start_time and end_time must be valid");
            return;
        }

        auto poller = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [val, invoker, headerOut, camera_id, stream_id,
                          start_time, end_time, protocol, poller, session]() mutable {
            // Segment/restore-job lookups and the per-pool health checks run on the API worker:
            // this endpoint sits on the playback path, where the latency is visible.
            dispatchApiWork(poller, session, headerOut, val, invoker,
                [camera_id, stream_id, start_time, end_time, protocol](Json::Value &val) {
                auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
                if (!device) {
                    throw ApiResponseException(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                }

                auto pool_map = TierStorageBase::loadPoolMap();
                SegmentTierRangeImp range_imp;
                auto ranges = range_imp.queryByCamera(camera_id, start_time, end_time);

                std::vector<SegmentTierRange> matched;
                matched.reserve(ranges.size());
                for (const auto &r : ranges) {
                    if (!stream_id.empty() && r.stream_id != stream_id)
                        continue;
                    if (r.end_time <= start_time || r.start_time >= end_time)
                        continue;
                    matched.push_back(r);
                }

                if (matched.empty()) {
                    Json::Value data;
                    data["status"] = "NOT_FOUND";
                    data["tier"] = "";
                    data["restore_required"] = false;
                    data["job_created"] = false;
                    data["protocol"] = protocol;
                    val["data"] = data;
                    throw ApiResponseException(ApiErrCode::CODE_STORAGE_SEGMENTS_NOT_FOUND, "No segments found for the specified time range");
                }

                std::string tier = matched.front().tier;
                bool restore_required = false;
                bool has_expired = false;
                bool has_deleted = false;
                bool has_missing = false;
                for (const auto &r : matched) {
                    if (r.status == segmentStatusToString(SegmentStatus::EXPIRED)) has_expired = true;
                    if (r.status == segmentStatusToString(SegmentStatus::DELETED)) has_deleted = true;
                    if (r.status == segmentStatusToString(SegmentStatus::MISSING)) has_missing = true;
                    if (TierStorageBase::isRestoreRequiredPool(r.pool_id, pool_map)) {
                        restore_required = true;
                        tier = r.tier;
                    }
                }

                if (has_expired || has_deleted || has_missing) {
                    Json::Value data;
                    data["status"] = has_expired ? "EXPIRED" : (has_deleted ? "DELETED" : "MISSING");
                    data["tier"] = tier;
                    data["restore_required"] = false;
                    data["job_created"] = false;
                    data["protocol"] = protocol;
                    val["data"] = data;
                    throw ApiResponseException(has_expired ? ApiErrCode::CODE_STORAGE_SEGMENTS_EXPIRED
                                                           : ApiErrCode::CODE_STORAGE_SEGMENTS_NOT_FOUND,
                                               has_expired ? "Some segments have expired"
                                                           : "Some segments are not available");
                }

                for (const auto &r : matched) {
                    if (pool_map.find(r.pool_id) == pool_map.end()) {
                        Json::Value data;
                        data["status"] = "POOL_NOT_FOUND";
                        data["tier"] = r.tier;
                        data["pool_id"] = r.pool_id;
                        data["restore_required"] = false;
                        data["job_created"] = false;
                        data["protocol"] = protocol;
                        val["data"] = data;
                        throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool referenced by segment metadata was not found");
                    }
                }

                std::vector<SegmentTierRange> restore_ranges;
                for (const auto &r : matched) {
                    const auto pool_it = pool_map.find(r.pool_id);
                    if (pool_it != pool_map.end() && poolTypeIsObjectStorage(pool_it->second.type))
                        restore_ranges.push_back(r);
                }

                RestoreJobImp restore_imp;
                auto jobs = restore_imp.queryOverlapping(camera_id, start_time, end_time, 0, stream_id);
                const RestoreJob *covering_done = nullptr;
                const RestoreJob *active_job = nullptr;
                const RestoreJob *latest_job = nullptr;
                int64_t restore_expires_at = 0;
                GET_CONFIG(int64_t, restore_ttl_seconds, Storage::kRestoreTTLSeconds);
                for (const auto &j : jobs) {
                    if (!latest_job)
                        latest_job = &j;
                    if (j.start_time <= start_time && j.end_time >= end_time &&
                        TierStorageManager::Instance().validateRestoreWindowCache(
                            j, restore_ranges, stream_id, start_time, end_time,
                            restore_ttl_seconds, restore_expires_at)) {
                        covering_done = &j;
                        break;
                    }
                    if (!active_job &&
                        (j.status == jobStatusToString(JobStatus::PENDING) ||
                        j.status == jobStatusToString(JobStatus::RUNNING))) {
                        active_job = &j;
                    }
                }

                Json::Value unavailable_pools = Json::arrayValue;
                Json::Value unavailable_pool_ids = Json::arrayValue;
                std::set<std::string> checked_pool_ids;
                bool has_unknown_pool = false;
                for (const auto &r : matched) {
                    if (!checked_pool_ids.insert(r.pool_id).second)
                        continue;
                    const auto pool_it = pool_map.find(r.pool_id);
                    const StoragePool &pool = pool_it->second;
                    if (covering_done && poolTypeIsObjectStorage(pool.type))
                        continue;

                    PoolHealthInfo health;
                    if (TierStorageManager::Instance().isPoolAvailableForPlayback(pool, &health))
                        continue;

                    Json::Value item;
                    item["pool_id"] = r.pool_id;
                    item["health_status"] = health.health_status;
                    item["last_health_check"] = static_cast<Json::Int64>(health.timestamp);
                    unavailable_pools.append(item);
                    unavailable_pool_ids.append(r.pool_id);
                    if (health.stale || health.health_status == healthToString(StorageHealth::UNKNOWN))
                        has_unknown_pool = true;
                }

                if (!unavailable_pools.empty()) {
                    Json::Value data;
                    data["status"] = has_unknown_pool ? "STORAGE_STATUS_UNKNOWN" : "STORAGE_OFFLINE";
                    data["tier"] = tier;
                    data["restore_required"] = false;
                    data["job_created"] = false;
                    data["retryable"] = true;
                    data["offline_pool_ids"] = unavailable_pool_ids;
                    data["pools"] = unavailable_pools;
                    data["protocol"] = protocol;
                    val["data"] = data;
                    throw ApiResponseException(ApiErrCode::CODE_STORAGE_POOL_OFFLINE,
                                               has_unknown_pool ? "Storage pool health status is unknown or stale"
                                                                : "Storage pool is offline");
                }

                if (restore_required && !covering_done) {
                    Json::Value data;
                    data["status"] = active_job ? "RESTORING" : "RESTORE_REQUIRED";
                    data["tier"] = tier.empty() ? tierTypeToString(ColdTier) : tier;
                    data["restore_required"] = true;
                    data["job_created"] = active_job != nullptr;
                    data["protocol"] = protocol;
                    if (active_job) {
                        data["job_id"] = active_job->job_id;
                        data["restore_job_status"] = active_job->status;
                    } else if (latest_job) {
                        data["job_id"] = latest_job->job_id;
                        data["restore_job_status"] = latest_job->status;
                    } else {
                        data["job_id"] = "";
                        data["restore_job_status"] = "";
                    }
                    data["estimated_restore_seconds"] = 120;
                    val["data"] = data;
                    throw ApiResponseException(ApiErrCode::CODE_STORAGE_RESTORE_REQUIRED,
                                               active_job ? "Restore job is running"
                                                          : "Recording is in object storage and must be restored before playback");
                }

                Json::Value data;
                data["status"] = "READY";
                data["tier"] = tier;
                data["restore_required"] = false;
                data["job_created"] = covering_done != nullptr;
                data["protocol"] = protocol;
                if (covering_done) {
                    data["job_id"] = covering_done->job_id;
                    data["restore_job_status"] = covering_done->status;
                }
                data["expires_at"] = static_cast<Json::Int64>(covering_done ? restore_expires_at : 0);
                val["data"] = data;
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(camera_id, on_access);
    });

    api_regist("/media/mserver/storage/restoreJob/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE, PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("camera_id", "start_time", "end_time");

        string camera_id = allArgs["camera_id"];
        string stream_id = allArgs["stream_id"];
        int64_t start_time = allArgs["start_time"];
        int64_t end_time = allArgs["end_time"];
        string target_tier = allArgs["target_tier"];
        string reason = allArgs["reason"];

        if (target_tier.empty()) {
            target_tier = "HOT";
        }
        if (target_tier != "HOT") {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "Only HOT restore target is supported");
            return;
        }
        if (start_time <= 0 || end_time <= 0 || start_time >= end_time) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_TIME_RANGE, "start_time and end_time must be valid");
            return;
        }

        auto poller = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [val, invoker, headerOut, camera_id, stream_id,
                          start_time, end_time, reason, poller, session]() mutable {
            // Restore planning runs on the API worker, serialized with the other storage mutations.
            dispatchApiWork(poller, session, headerOut, val, invoker,
                [camera_id, stream_id, start_time, end_time, reason](Json::Value &val) {
                auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
                if (!device) {
                    throw ApiResponseException(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                }

                auto restore = TierStorageManager::Instance().requestRestoreWindow(
                    camera_id, stream_id, start_time, end_time,
                    reason.empty() ? "MANUAL" : reason);
                if (!restore.handled) {
                    throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                               "Failed to create restore job");
                }

                RestoreJobSnapshot snapshot;
                if (!TierStorageManager::Instance().getRestoreJobSnapshot(
                        restore.job_id, snapshot)) {
                    throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION,
                                               "Restore job was created but its status is unavailable");
                }
                Json::Value data = TierStorageBase::restoreJobSnapshotToJson(snapshot);
                data["job_created"] = restore.job_created;
                val["data"] = data;
            }, nullptr, apiWorkSerial("storage.mutation"));
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(camera_id, on_access);
    });

    api_regist("/media/mserver/storage/restoreJob/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE, PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("job_id");

        string job_id = allArgs["job_id"];
        auto poller = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);

        // The job lookup and its snapshot run on the API worker; the device access check stays on
        // the request poller between them.
        auto camera_id = std::make_shared<string>();
        runApiWork(poller, session,
            [job_id, camera_id]() {
                auto jobs = TierStorageManager::Instance().getRestoreJob(job_id);
                if (jobs.empty()) {
                    throw ApiResponseException(ApiErrCode::CODE_STORAGE_RESTORE_JOB_NOT_FOUND, "Restore job not found");
                }
                *camera_id = jobs.front().camera_id;
            },
            [allArgs, val, invoker, headerOut, enable_authorize, job_id, camera_id, poller, session](const exception_ptr &error) mutable {
            if (error) {
                responseApiException(error, invoker, session);
                return;
            }
            auto on_access = [val, invoker, headerOut, job_id, poller, session]() mutable {
                dispatchApiWork(poller, session, headerOut, val, invoker, [job_id](Json::Value &val) {
                    RestoreJobSnapshot snapshot;
                    if (!TierStorageManager::Instance().getRestoreJobSnapshot(job_id, snapshot)) {
                        throw ApiResponseException(ApiErrCode::CODE_STORAGE_RESTORE_JOB_NOT_FOUND,
                                                   "Restore job not found");
                    }
                    val["data"] = TierStorageBase::restoreJobSnapshotToJson(snapshot);
                }, nullptr, apiWorkLimit("storage.read"));
            };

            CHECK_USER_DEVICE_AUTHOR_ASYNC(*camera_id, on_access);
        });
    });

    api_regist("/media/mserver/storage/restoreJob/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE, PLAYBACK_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageRestoreJobList(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // ================================================================
    // === Section 6: Tiering Job APIs ================================
    // ================================================================

    api_regist("/media/mserver/storage/tieringJob/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageTieringJobList(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    api_regist("/media/mserver/storage/tieringJob/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("job_id");

        // Repository queries and JSON building run on the API worker.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            string job_id = allArgs["job_id"];
            auto jobs = TierStorageManager::Instance().getTieringJob(job_id);
            if (jobs.empty()) {
                throw ApiResponseException(ApiErrCode::CODE_STORAGE_TIERING_JOB_NOT_FOUND, "Tiering job not found");
            }
            val["data"] = TierStorageBase::tieringJobToJson(jobs.front(), "", true);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    api_regist("/media/mserver/storage/tieringJob/retry", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("job_id");

        // Mutations run on the API worker, one at a time: they update the DB and the shared pool/policy caches and registrations.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            string job_id = allArgs["job_id"];

            auto jobs = TierStorageManager::Instance().getTieringJob(job_id);
            if (jobs.empty()) {
                throw ApiResponseException(ApiErrCode::CODE_STORAGE_TIERING_JOB_NOT_FOUND, "Tiering job not found");
            }
            if (!TierStorageManager::Instance().retryTieringJob(job_id)) {
                throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Failed to retry tiering job");
            }   
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    api_regist("/media/mserver/storage/tieringJob/cancel", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("job_id");

        // Mutations run on the API worker, one at a time: they update the DB and the shared pool/policy caches and registrations.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            string job_id = allArgs["job_id"];
            if (TierStorageManager::Instance().getTieringJob(job_id).empty()) {
                throw ApiResponseException(ApiErrCode::CODE_STORAGE_TIERING_JOB_NOT_FOUND, "Tiering job not found");
            }
            if (!TierStorageManager::Instance().cancelTieringJob(job_id)) {
                throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Failed to cancel tiering job");
            }
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // ================================================================
    // === Section 7: Storage Dashboard APIs ==========================
    // ================================================================

    api_regist("/media/mserver/storage/dashboard/summary", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [](Json::Value &val) {
            storageDashboardSummary(val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    api_regist("/media/mserver/storage/dashboard/detail", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [](Json::Value &val) {
            storageDashboardDetail(val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // ================================================================
    // === Section 8: Alert APIs ======================================
    // ================================================================

    api_regist("/media/mserver/storage/alert/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageAlertList(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    api_regist("/media/mserver/storage/alert/ack", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("alert_id");

        // Mutations run on the API worker, one at a time: they update the DB and the shared pool/policy caches and registrations.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            string alert_id = allArgs["alert_id"];
            StorageAlertImp imp;
            if (!imp.acknowledge(alert_id)) {
                throw ApiResponseException(ApiErrCode::CODE_STORAGE_ALERT_NOT_FOUND, "Alert not found or could not be acknowledged");
            }
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // ================================================================
    // === Section 9: Expired / Delete Approval APIs ==================
    // ================================================================

    api_regist("/media/mserver/storage/expiredSegment/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageExpiredSegmentList(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    api_regist("/media/mserver/storage/expiredSegment/approve", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);

        // Mutations run on the API worker, one at a time: they update the DB and the shared pool/policy caches and registrations.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            Json::Value range_ids = allArgs.getArgs()["range_ids"];
            if (range_ids.empty() || !range_ids.isArray()) {
                throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, "range_ids must be an array");
            }

            int approved = 0;
            for (const auto &idv : range_ids) {
                std::string range_id = idv.asString();
                if (range_id.empty())
                    continue;
                if (TierStorageManager::Instance().approveExpiredRange(range_id))
                    ++approved;
            }

            Json::Value data;
            data["approved_count"] = approved;
            val["data"] = data;
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    api_regist("/media/mserver/storage/expiredSegment/extend", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);

        // Mutations run on the API worker, one at a time: they update the DB and the shared pool/policy caches and registrations.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            Json::Value range_ids = allArgs.getArgs()["range_ids"];
            if (range_ids.empty())
                range_ids = allArgs.getArgs()["segment_ids"];
            int64_t extend_until = allArgs["extend_until"].empty() ? 0 : std::stoll(allArgs["extend_until"]);

            if (range_ids.empty() || !range_ids.isArray()) {
                throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, "range_ids must be an array");
            }

            if (extend_until <= 0) {
                throw ApiResponseException(ApiErrCode::CODE_INVALID_ARGS, "extend_until is required");
            }

            std::string reason = allArgs["reason"];
            int extended = 0;
            for (const auto &idv : range_ids) {
                std::string range_id = idv.asString();
                if (range_id.empty())
                    continue;
                if (TierStorageManager::Instance().extendRangeRetention(range_id, extend_until, reason))
                    ++extended;
            }

            Json::Value data;
            data["extended_count"] = extended;
            val["data"] = data;
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    // ================================================================
    // === Section 10: Protected Video APIs ===========================
    // ================================================================

    api_regist("/media/mserver/storage/protected/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE, MODIFY_MSERVER_PERMISSION_CODE);

        // Mutations run on the API worker, one at a time: they update the DB and the shared pool/policy caches and registrations.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            std::string camera_id = allArgs["camera_id"];
            int64_t start_time = allArgs["start_time"].empty() ? 0 : std::stoll(allArgs["start_time"]);
            int64_t end_time = allArgs["end_time"].empty() ? 0 : std::stoll(allArgs["end_time"]);
            std::string type = allArgs["type"].empty() ? "PROTECTED" : allArgs["type"];
            std::string reason = allArgs["reason"];

            if (start_time <= 0 || end_time <= 0 || start_time >= end_time) {
                throw ApiResponseException(ApiErrCode::CODE_INVALID_TIME_RANGE, "start_time and end_time must be valid");
            }

            ProtectedVideo video;
            video.protected_id = StrUUID::make_guid(8, "prot");
            video.camera_id = camera_id;
            video.start_time = start_time;
            video.end_time = end_time;
            video.type = type.empty() ? "PROTECTED" : type;
            video.reason = Optional<std::string>(reason);
            video.created_at = static_cast<int64_t>(time(nullptr));

            ProtectedVideoImp imp;
            if (!imp.add(video)) {
                throw ApiResponseException(ApiErrCode::CODE_OTHER_EXCEPTION, "Failed to create protected video");
            }

            Json::Value data;
            data["protected_id"] = video.protected_id;

            val["data"] = data;
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    api_regist("/media/mserver/storage/protected/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE, MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("protected_id");

        // Mutations run on the API worker, one at a time: they update the DB and the shared pool/policy caches and registrations.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            std::string protected_id = allArgs["protected_id"];

            ProtectedVideoImp imp;
            if (!imp.remove(protected_id)) {
                throw ApiResponseException(ApiErrCode::CODE_STORAGE_SEGMENT_PROTECTED_NOT_FOUND, "Failed to delete protected video");
            }
        }, nullptr, apiWorkSerial("storage.mutation"));
    });

    api_regist("/media/mserver/storage/protected/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE, READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageProtectedList(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    // Constant answer: no blocking work, so no dispatch.
    api_regist("/media/mserver/storage/pool/options", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        storagePoolOptions(val);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/mountpoint/available", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            storageMountpointAvailable(allArgs, val);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    InfoL << "Storage tiering APIs registered";
}

} // namespace managerkit

#else

namespace managerkit {

void registerStorageApis() {
    WarnL << "Storage tiering APIs are disabled because ENABLE_STORAGE_TIERING is not defined";
}

} // namespace managerkit

#endif // ENABLE_STORAGE_TIERING
