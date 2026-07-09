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
 */

#include <algorithm>

#include "Manager.h"
#include "WebApi.h"
#include "WebApiErrCode.h"
#include "Camera/CameraManager.h"
#include "Common/StrUtil.h"
#include "Local/TierStorageManager.h"
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


// ===================================================================
// Policy tier config validation helper
// ===================================================================
static bool validatePolicyTierMoveChain(const std::vector<PolicyTierConfig> &tiers, std::string &err) {
    std::vector<PolicyTierConfig> sorted;
    for (const auto &tier : tiers) {
        if (tier.enabled)
            sorted.push_back(tier);
    }
    std::sort(sorted.begin(), sorted.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
        return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
    });
    if (sorted.empty() || sorted.front().tier != "HOT") {
        err = "HOT tier must be enabled";
        return false;
    }
    for (size_t i = 0; i + 1 < sorted.size(); ++i) {
        if (sorted[i].overflow_action != "MOVE_TO_NEXT_TIER") {
            err = sorted[i + 1].tier + " tier can only be enabled when " +
                  sorted[i].tier + " overflow_action is MOVE_TO_NEXT_TIER";
            return false;
        }
    }
    return true;
}

static bool validatePolicyTiers(const std::vector<PolicyTierConfig> &tiers, std::string &err) {
    if (!validatePolicyTierMoveChain(tiers, err))
        return false;

    // Enabled tiers must increase by retain_until_days in HOT -> WARM -> COLD order.
    std::vector<PolicyTierConfig> sorted;
    for (const auto &t : tiers) {
        if (t.enabled)
            sorted.push_back(t);
    }
    std::sort(sorted.begin(), sorted.end(), [](const PolicyTierConfig &a, const PolicyTierConfig &b) {
        return tierTypeFromString(a.tier) < tierTypeFromString(b.tier);
    });

    int previous_days = -1;
    std::string previous_tier;
    for (const auto &t : sorted) {
        if (previous_days >= 0 && previous_days >= t.retain_until_days) {
            err = previous_tier + ".retain_until_days must be less than " +
                  t.tier + ".retain_until_days";
            return false;
        }
        previous_days = t.retain_until_days;
        previous_tier = t.tier;
        if (t.high_watermark_percent >= t.critical_watermark_percent) {
            err = "high_watermark_percent must be less than critical_watermark_percent";
            return false;
        }
    }
    return true;
}

static std::unordered_map<std::string, StoragePool> loadPoolMap() {
    StoragePoolImp imp;
    std::unordered_map<std::string, StoragePool> ret;
    for (const auto &pool : imp.queryAll())
        ret[pool.id] = pool;
    return ret;
}

static bool isRestoreRequiredPool(const std::string &pool_id,
                                  const std::unordered_map<std::string, StoragePool> &pool_map) {
    auto it = pool_map.find(pool_id);
    return it != pool_map.end() && poolTypeIsObjectStorage(it->second.type);
}

static Json::Value poolToDocJson(const StoragePool &p) {
    Json::Value v;
    const int64_t free_bytes = p.total_bytes > p.used_bytes ? p.total_bytes - p.used_bytes : 0;
    const int used_percent = p.total_bytes > 0
        ? static_cast<int>((p.used_bytes * 100) / p.total_bytes)
        : static_cast<int>(p.usage_pct);

    v["id"]                = p.id;
    v["name"]              = p.name;
    v["type"]              = p.type;
    v["tier"]              = p.tier;
    v["status"]            = p.health_status.empty() ? "OK" : p.health_status;
    v["total_bytes"]       = static_cast<Json::Int64>(p.total_bytes);
    v["used_bytes"]        = static_cast<Json::Int64>(p.used_bytes);
    v["free_bytes"]        = static_cast<Json::Int64>(free_bytes);
    v["used_percent"]      = used_percent;
    v["write_mbps"]        = 0;
    v["read_mbps"]         = 0;
    v["camera_count"]      = 0;
    v["enabled"]           = (p.enabled != 0);
    v["last_health_check"] = static_cast<Json::Int64>(p.last_health_check);
    return v;
}

static Json::Value poolDetailToDocJson(const StoragePool &p) {
    Json::Value v = poolToDocJson(p);
    const double used_percent = p.total_bytes > 0
        ? static_cast<double>(p.used_bytes) * 100.0 / static_cast<double>(p.total_bytes)
        : static_cast<double>(p.usage_pct);

    v["access_key"]                 = p.access_key.has_value() && !p.access_key.value().empty()
                                      ? "***********" : "";
    v["secret_key"]                 = p.secret_key_enc.has_value() && !p.secret_key_enc.value().empty()
                                      ? "***********" : "";
    v["base_path"]                  = p.base_path.value_or("");
    v["bucket"]                     = p.bucket.value_or("");
    v["created_at"]                 = static_cast<Json::Int64>(p.created_at);
    v["critical_watermark_percent"] = p.critical_watermark_percent;
    v["endpoint"]                   = p.endpoint.value_or("");
    v["health_check_enabled"]       = (p.health_check_enabled != 0);
    v["high_watermark_percent"]     = p.high_watermark_percent;
    v["mount_path"]                 = p.mount_path.value_or("");
    v["network_path"]               = p.network_path.value_or("");
    v["updated_at"]                 = static_cast<Json::Int64>(p.updated_at);
    v["used_percent"]               = used_percent;
    return v;
}

static bool validatePoolByType(const StoragePool &pool, std::string &err) {
    auto support = poolTypeSupport(tierTypeFromString(pool.tier));
    if (support.isMember(pool.type) && !support[pool.type].asBool()) {
        err = pool.type + " pool is not supported for " + pool.tier + " tier";
        return false;
    }
    if (pool.type == "LOCAL_DISK") {
        if (!pool.mount_path.has_value() || pool.mount_path.value().empty()) {
            err = "mount_path is required for LOCAL_DISK";
            return false;
        }
    } else if (pool.type == "NAS") {
        const bool has_mount = pool.mount_path.has_value() && !pool.mount_path.value().empty();
        const bool has_network = pool.network_path.has_value() && !pool.network_path.value().empty();
        if (!has_mount && !has_network) {
            err = "mount_path or network_path is required for NAS";
            return false;
        }
    } else if (pool.type == "MINIO" || pool.type == "S3") {
        if (!pool.endpoint.has_value() || pool.endpoint.value().empty() ||
            !pool.bucket.has_value() || pool.bucket.value().empty() ||
            !pool.base_path.has_value() || pool.base_path.value().empty() ||
            !pool.access_key.has_value() || pool.access_key.value().empty() ||
            !pool.secret_key_enc.has_value() || pool.secret_key_enc.value().empty()) {
            err = "endpoint, bucket, base_path, access_key and secret_key are required for MINIO/S3";
            return false;
        }
    }
    return true;
}

static Json::Value policyDetailToDocJson(
    const StoragePolicy &p,
    const std::unordered_map<std::string, StoragePool> &pool_map) {
    Json::Value v;
    v["id"]                    = p.id;
    v["name"]                  = p.name;
    v["description"]           = p.description.value_or("");
    v["enabled"]               = (p.enabled != 0);
    v["total_retention_days"]  = p.total_retention_days;
    v["allow_camera_override"] = (p.allow_camera_override != 0);
    v["protect_event_video"]   = (p.protect_event_video != 0);

    Json::Value tiers(Json::arrayValue);
    for (const auto &tier : p.parsedTiers()) {
        auto tv = tier.toJson();
        auto it = pool_map.find(tier.pool_id);
        tv["pool_name"] = it != pool_map.end() ? it->second.name : "";
        tiers.append(tv);
    }
    v["tiers"] = tiers;
    v["delete_policy"] = p.parsedDeletePolicy().toJson();
    v["advanced_rules"] = p.parsedAdvancedRules().toJson();
    return v;
}

static Json::Value policySummaryToDocJson(const StoragePolicy &p, int applied_camera_count) {
    Json::Value v;
    v["id"]                    = p.id;
    v["name"]                  = p.name;
    v["description"]           = p.description.value_or("");
    v["enabled"]               = (p.enabled != 0);
    v["total_retention_days"]  = p.total_retention_days;
    v["hot_retain_until_days"] = 0;
    v["warm_retain_until_days"] = 0;
    v["cold_retain_until_days"] = 0;
    for (const auto &tier : p.parsedTiers()) {
        if (tier.tier == "HOT") v["hot_retain_until_days"] = tier.retain_until_days;
        if (tier.tier == "WARM") v["warm_retain_until_days"] = tier.retain_until_days;
        if (tier.tier == "COLD") v["cold_retain_until_days"] = tier.retain_until_days;
    }
    v["delete_after_days"]     = p.parsedDeletePolicy().delete_after_days;
    v["allow_camera_override"] = (p.allow_camera_override != 0);
    v["protect_event_video"]   = (p.protect_event_video != 0);
    v["applied_camera_count"]  = applied_camera_count;
    v["created_at"]            = static_cast<Json::Int64>(p.created_at);
    v["updated_at"]            = static_cast<Json::Int64>(p.updated_at);
    return v;
}

static bool validatePolicyByDocRules(const StoragePolicy &policy, std::string &err) {
    auto tiers = policy.parsedTiers();
    if (!validatePolicyTiers(tiers, err))
        return false;

    int max_retain_days = 0;
    bool hot_found = false;
    bool hot_enabled = false;
    auto pool_map = loadPoolMap();

    for (const auto &tier : tiers) {
        if (!tier.enabled) continue;
        max_retain_days = std::max(max_retain_days, tier.retain_until_days);
        if (tier.tier == "HOT") {
            hot_found = true;
            hot_enabled = true;
        }
        if (tier.pool_id.empty()) {
            err = tier.tier + ".pool_id is required";
            return false;
        }
        auto pit = pool_map.find(tier.pool_id);
        if (pit == pool_map.end() || pit->second.enabled == 0) {
            err = "pool_id must exist and be enabled: " + tier.pool_id;
            return false;
        }
        if (pit->second.tier != tier.tier) {
            err = "pool_id " + tier.pool_id + " belongs to " + pit->second.tier +
                  " tier, expected " + tier.tier;
            return false;
        }
        auto support = poolTypeSupport(tierTypeFromString(tier.tier));
        if (support.isMember(pit->second.type) && !support[pit->second.type].asBool()) {
            err = pit->second.type + " pool is not supported for " + tier.tier + " tier";
            return false;
        }
        if (tier.high_watermark_percent >= tier.critical_watermark_percent) {
            err = "high_watermark_percent must be less than critical_watermark_percent";
            return false;
        }
        if (tier.critical_watermark_percent > 95) {
            err = "critical_watermark_percent must be <= 95";
            return false;
        }
    }

    if (!hot_found || !hot_enabled) {
        err = "HOT tier must be enabled";
        return false;
    }

    const auto delete_policy = policy.parsedDeletePolicy();
    if (delete_policy.delete_after_days < max_retain_days) {
        err = "delete_after_days must be greater than or equal to the largest retain_until_days";
        return false;
    }
    return true;
}

static int64_t latestTieringJobTime(const std::string &camera_id) {
    TieringJobImp imp;
    auto jobs = imp.query(camera_id, "", "", "", 0, 0, 0, 1);
    if (jobs.empty()) return 0;
    return jobs.front().updated_at > 0 ? jobs.front().updated_at : jobs.front().created_at;
}

static Json::Value effectivePolicyToJson(const EffectivePolicyResult &result, std::string camera_name) {
    Json::Value data;
    bool is_camera_override = result.source == policySourceToString(PolicySource::CAMERA);
    data["camera_id"]             = result.camera_id;
    data["camera_name"]           = camera_name;
    data["policy_id"]             = is_camera_override ? result.policy_id : "";
    data["effective_policy_id"]   = result.policy_id;
    data["policy_name"]           = result.policy_name;
    data["source"]                = result.source;
    data["source_id"]             = is_camera_override ? result.camera_id : "";
    data["allow_camera_override"] = result.allow_camera_override;
    return data;
}

static int progressPercent(int64_t processed, int64_t total, const std::string &status) {
    if (status == "DONE") return 100;
    if (total <= 0) return processed > 0 ? 100 : 0;
    auto pct = static_cast<int>((processed * 100) / total);
    return std::max(0, std::min(100, pct));
}

static Json::Value restoreJobToDocJson(const RestoreJob &j, bool list_view = false) {
    Json::Value v;
    v["job_id"]           = j.job_id;
    v["status"]           = j.status;
    v["progress_percent"] = progressPercent(j.processed_bytes, j.total_bytes, j.status);
    v["camera_id"]        = j.camera_id;
    if (list_view) {
        auto device = findDeviceSource(j.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        v["camera_name"] = device ? device->getDeviceTuple().name : "";
    } else
        v["type"] = "RESTORE";
    v["source_tier"]      = j.source_tier;
    v["target_tier"]      = j.target_tier;
    if (!list_view) {
        v["start_time"]      = static_cast<Json::Int64>(j.start_time);
        v["end_time"]        = static_cast<Json::Int64>(j.end_time);
        v["processed_bytes"] = static_cast<Json::Int64>(j.processed_bytes);
    }
    v["total_bytes"]      = static_cast<Json::Int64>(j.total_bytes);
    v["playback_ready"]   = (j.status == "DONE");
    v["created_at"]       = static_cast<Json::Int64>(j.created_at);
    if (!list_view)
        v["updated_at"] = static_cast<Json::Int64>(j.updated_at);
    return v;
}

static Json::Value tieringJobToDocJson(const TieringJob &j, bool detail = false) {
    Json::Value v;
    v["job_id"]                  = j.job_id;
    v["status"]                  = j.status;
    v["progress_percent"]        = progressPercent(j.bytes_moved, j.bytes_total, j.status);
    v["camera_id"]               = j.camera_id;
    v["stream_id"]               = j.stream_id;
    v["range_id"]                = j.range_id;
    if (!detail) {
        auto device = findDeviceSource(j.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        v["camera_name"] = device ? device->getDeviceTuple().name : "";
    }
    v["source_tier"]             = j.source_tier;
    v["target_tier"]             = j.target_tier;
    v["source_pool_id"]          = j.source_pool_id;
    v["target_pool_id"]          = j.target_pool_id;
    v["segment_count"]           = 0;
    v["processed_segment_count"] = 0;
    v["total_bytes"]             = static_cast<Json::Int64>(j.bytes_total);
    v["processed_bytes"]         = static_cast<Json::Int64>(j.bytes_moved);
    v["created_at"]              = static_cast<Json::Int64>(j.created_at);
    v["updated_at"]              = static_cast<Json::Int64>(j.updated_at);
    if (detail) {
        const auto error = j.error_message.value_or("");
        v["error_code"] = error.empty() ? "" : "TIERING_JOB_FAILED";
        v["error_message"] = error;
        v["retryable"] = (j.status == "FAILED");
    }
    return v;
}

static std::string worstStatus(const std::string &a, const std::string &b) {
    auto rank = [](const std::string &s) {
        if (s == "CRITICAL" || s == "OFFLINE") return 4;
        if (s == "HIGH") return 3;
        if (s == "WARNING") return 2;
        return 1;
    };
    return rank(b) > rank(a) ? b : a;
}

static Json::Value dashboardSummaryToDocJson() {
    auto pools = TierStorageManager::Instance().listPools();

    struct TierAgg {
        int64_t total = 0;
        int64_t used = 0;
        std::string status = "OK";
    };
    struct TierAggCompare {
        bool operator()(const std::string &x, const std::string &y) const { return tierTypeFromString(x) < tierTypeFromString(y); }
    };
    std::map<std::string, TierAgg, TierAggCompare> tiers;
    tiers["HOT"];
    tiers["WARM"];
    tiers["COLD"];

    int64_t total_bytes = 0;
    int64_t used_bytes = 0;
    std::string status = "OK";
    Json::Value alerts(Json::arrayValue);

    for (const auto &p : pools) {
        const std::string pool_status = p.health_status.empty() ? "OK" : p.health_status;
        auto &agg = tiers[p.tier];
        agg.total += p.total_bytes;
        agg.used += p.used_bytes;
        agg.status = worstStatus(agg.status, pool_status);
        total_bytes += p.total_bytes;
        used_bytes += p.used_bytes;
        status = worstStatus(status, pool_status);

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
        t["estimated_remaining_days"] = 0.0;
        t["write_mbps"] = 0;
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
    data["tier_summary"] = tier_summary;
    data["active_tiering_jobs"] = tiering_imp.countQuery("", "PENDING") + tiering_imp.countQuery("", "RUNNING");
    data["failed_tiering_jobs"] = tiering_imp.countQuery("", "FAILED");
    data["active_restore_jobs"] = restore_imp.countActive();
    data["alerts"] = alerts;
    return data;
}

static Json::Value alertToDocJson(const StorageAlert &a, const std::unordered_map<std::string, StoragePool> &pool_map) {
    Json::Value v;
    v["id"] = a.id;
    v["level"] = a.level;
    v["type"] = a.type;
    v["pool_id"] = a.pool_id;
    auto it = pool_map.find(a.pool_id);
    v["pool_name"] = it == pool_map.end() ? "" : it->second.name;
    v["message"] = a.message;
    v["created_at"] = static_cast<Json::Int64>(a.created_at);
    v["acknowledged"] = (a.acknowledged != 0);
    return v;
}

static Json::Value expiredSegmentToDocJson(const SegmentTierRange &r) {
    ProtectedVideoImp protected_imp;
    Json::Value v;
    v["segment_id"] = r.range_id;
    v["range_id"] = r.range_id;
    v["camera_id"] = r.camera_id;
    v["stream_id"] = r.stream_id;
    {
        auto device = findDeviceSource(r.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        v["camera_name"] = device ? device->getDeviceTuple().name : "";
    }
    v["start_time"] = static_cast<Json::Int64>(r.start_time);
    v["end_time"] = static_cast<Json::Int64>(r.end_time);
    v["tier"] = r.tier;
    v["pool_id"] = r.pool_id;
    v["segment_count"] = static_cast<Json::Int64>(r.segment_count);
    v["size_bytes"] = static_cast<Json::Int64>(r.size_bytes);
    v["expired_at"] = static_cast<Json::Int64>(r.updated_at > 0 ? r.updated_at : r.end_time);
    v["protected"] = protected_imp.overlaps(r.camera_id, r.start_time, r.end_time);
    v["evidence"] = false;
    return v;
}

static Json::Value protectedVideoToDocJson(const ProtectedVideo &p) {
    Json::Value v;
    v["protected_id"] = p.protected_id;
    v["camera_id"] = p.camera_id;
    {
        auto device = findDeviceSource(p.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        v["camera_name"] = device ? device->getDeviceTuple().name : "";
    }
    v["start_time"] = static_cast<Json::Int64>(p.start_time);
    v["end_time"] = static_cast<Json::Int64>(p.end_time);
    v["type"] = p.type;
    v["reason"] = p.reason.value_or("");
    v["created_at"] = static_cast<Json::Int64>(p.created_at);
    return v;
}

// ===================================================================
// Build a StoragePool from JSON request body
// ===================================================================
static StoragePool poolFromJson(const Json::Value &body) {
    StoragePool p;
    p.id   = body.get("id",   "").asString();
    p.name = body.get("name", "").asString();
    p.type = body.get("type", "").asString();
    p.tier = body.get("tier", "").asString();

    auto optStr = [&](const char *key) -> Optional<std::string> {
        if (body.isMember(key) && !body[key].isNull()) {
            std::string v = body[key].asString();
            if (!v.empty()) return Optional<std::string>(v);
        }
        return Optional<std::string>();
    };

    p.endpoint      = optStr("endpoint");
    p.bucket        = optStr("bucket");
    p.base_path     = optStr("base_path");
    p.access_key    = optStr("access_key");
    p.mount_path    = optStr("mount_path");
    p.network_path  = optStr("network_path");

    if (body.isMember("secret_key") && !body["secret_key"].isNull()) {
        // In production: encrypt before storing
        p.secret_key_enc = Optional<std::string>(body["secret_key"].asString());
    }

    p.enabled                    = body.get("enabled", true).asBool() ? 1 : 0;
    p.health_check_enabled       = body.get("health_check_enabled", true).asBool() ? 1 : 0;
    p.high_watermark_percent     = body.get("high_watermark_percent", 85).asInt();
    p.critical_watermark_percent = body.get("critical_watermark_percent", 90).asInt();
    return p;
}

// ===================================================================
// Build a StoragePolicy from JSON request body
// ===================================================================
static StoragePolicy policyFromJson(const Json::Value &body) {
    StoragePolicy p;
    p.id             = body.get("id",   "").asString();
    p.name           = body.get("name", "").asString();
    if (body.isMember("description") && !body["description"].isNull())
        p.description = Optional<std::string>(body["description"].asString());
    p.enabled               = body.get("enabled", true).asBool() ? 1 : 0;
    p.total_retention_days  = body.get("total_retention_days", 30).asInt();
    p.allow_camera_override = body.get("allow_camera_override", true).asBool() ? 1 : 0;
    p.protect_event_video   = body.get("protect_event_video", false).asBool() ? 1 : 0;

    // Serialize nested objects back to JSON strings for storage
    Json::FastWriter writer;

    if (body.isMember("tiers") && body["tiers"].isArray()) {
        p.tiers_json = writer.write(body["tiers"]);
    } else {
        p.tiers_json = "[]";
    }

    if (body.isMember("delete_policy") && body["delete_policy"].isObject()) {
        p.delete_policy_json = writer.write(body["delete_policy"]);
    } else {
        p.delete_policy_json = "{}";
    }

    if (body.isMember("advanced_rules") && body["advanced_rules"].isObject()) {
        p.advanced_rules_json = writer.write(body["advanced_rules"]);
    } else {
        p.advanced_rules_json = "{}";
    }

    return p;
}

// ===================================================================
// Register all storage API routes
// ===================================================================
namespace managerkit {

void registerStorageApis() {

    // ================================================================
    // === Section 1: Storage Pool APIs ================================
    // ================================================================

    // GET/POST /media/mserver/storage/pool/list
    api_regist("/media/mserver/storage/pool/list", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

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
            data.append(poolToDocJson(p));
        }
        val["data"] = data;
    });

    // GET/POST /media/mserver/storage/pool/detail
    api_regist("/media/mserver/storage/pool/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string pool_id = allArgs["id"];
        auto pools = TierStorageManager::Instance().getPool(pool_id);
        if (pools.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
            return;
        }
        val["data"] = poolDetailToDocJson(pools.front());
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/pool/create
    api_regist("/media/mserver/storage/pool/create", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("name", "type", "tier");
    
        auto pool = poolFromJson(allArgs.getArgs());

        std::string pool_err;
        if (!validatePoolByType(pool, pool_err)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, pool_err);
            return;
        }

        // Validate watermarks
        if (pool.high_watermark_percent >= pool.critical_watermark_percent) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_WATERMARK_PERCENT, "high_watermark_percent must be less than critical_watermark_percent");
            return;
        }

        auto id = TierStorageManager::Instance().createPool(pool);
        if (id.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_CREATE_FAILED, "Failed to create storage pool");
            return;
        }

        val["data"]["id"] = id;
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/pool/update
    api_regist("/media/mserver/storage/pool/update", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string pool_id = allArgs["id"];

        auto existing = TierStorageManager::Instance().getPool(pool_id);
        if (existing.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
            return;
        }
        auto body = allArgs.getArgs();

        // Merge update onto existing record
        auto &base = existing.front();
        if (body.isMember("name") && !body["name"].asString().empty())
            base.name = body["name"].asString();
        if (body.isMember("enabled"))
            base.enabled = body["enabled"].asBool() ? 1 : 0;
        if (body.isMember("health_check_enabled"))
            base.health_check_enabled = body["health_check_enabled"].asBool() ? 1 : 0;
        if (body.isMember("high_watermark_percent"))
            base.high_watermark_percent = body["high_watermark_percent"].asInt();
        if (body.isMember("critical_watermark_percent"))
            base.critical_watermark_percent = body["critical_watermark_percent"].asInt();

        if (base.high_watermark_percent >= base.critical_watermark_percent) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_WATERMARK_PERCENT, "high_watermark_percent must be less than critical_watermark_percent");
            return;
        }
        std::string pool_err;
        if (!validatePoolByType(base, pool_err)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, pool_err);
            return;
        }

        if (!TierStorageManager::Instance().updatePool(base)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_UPDATE_FAILED, "Failed to update storage pool");
            return;
        }
        val["data"]["id"] = pool_id;
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/pool/delete
    api_regist("/media/mserver/storage/pool/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string pool_id = allArgs["id"];
        int ref_count = 0;
        bool is_default_pool = false;
        if (!TierStorageManager::Instance().deletePool(pool_id, ref_count, is_default_pool)) {
            val["data"]["ref_count"] = ref_count;
            val["data"]["is_default_pool"] = is_default_pool;
            if (is_default_pool) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_DEFAULT_CANNOT_DELETE, "Cannot delete system default storage pool");
                return;
            }
            if (ref_count > 0) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_IN_USE, "Storage pool is used by " + std::to_string(ref_count) + " policies");
                return;
            }
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_DELETE_FAILED, "Failed to delete storage pool");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/pool/testConnection
    api_regist("/media/mserver/storage/pool/testConnection", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);

        string pool_id = allArgs["pool_id"];
        StoragePool pool;
        if (!pool_id.empty()) {
            // Test an existing saved pool
            auto existing = TierStorageManager::Instance().getPool(pool_id);
            if (existing.empty()) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
                return;
            }
            pool = existing.front();
        } else {
            // Ad-hoc test (before pool is saved)
            pool = poolFromJson(allArgs.getArgs());
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
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_CONN_FAILED, message);
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    // ================================================================
    // === Section 2: Storage Policy APIs ==============================
    // ================================================================

    // GET/POST /media/mserver/storage/policy/list
    api_regist("/media/mserver/storage/policy/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        string keyword = allArgs["keyword"];
        int enabled_filter  = !allArgs["enabled"].empty() ? allArgs["enabled"].as<int>() : -1;
        int page = !allArgs["page"].empty() ? allArgs["page"].as<int>() : 0;
        int size = !allArgs["size"].empty() ? allArgs["size"].as<int>() : 20;

        auto policies = TierStorageManager::Instance().listPolicies(keyword, enabled_filter, page, size);
        auto total_count = TierStorageManager::Instance().countPolicies(keyword, enabled_filter);

        Json::Value items = Json::arrayValue;
        PolicyAssignmentImp assignment_imp;
        for (const auto &p : policies) {
            const int applied_count = static_cast<int>(assignment_imp.findCamerasByPolicyId(p.id).size());
            items.append(policySummaryToDocJson(p, applied_count));
        }

        Json::Value data;
        data["items"] = items;
        data["page"]  = page;
        data["size"]  = size;
        data["total"] = total_count;
        val["data"]   = data;
        invoker(200, headerOut, val.toStyledString());
    });

    // GET/POST /media/mserver/storage/policy/detail
    api_regist("/media/mserver/storage/policy/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string policy_id = allArgs["id"];

        auto policies = TierStorageManager::Instance().getPolicy(policy_id);
        if (policies.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }

        val["data"] = policyDetailToDocJson(policies.front(), loadPoolMap());
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/policy/create
    api_regist("/media/mserver/storage/policy/create", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("name", "total_retention_days");

        auto policy = policyFromJson(allArgs.getArgs());

        std::string tier_err;
        if (!validatePolicyByDocRules(policy, tier_err)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, tier_err);
            return;
        }

        auto id = TierStorageManager::Instance().createPolicy(policy);
        if (id.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_CREATE_FAILED, "Failed to create storage policy");
            return;
        }

        val["data"]["id"] = id;
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/policy/update
    api_regist("/media/mserver/storage/policy/update", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string policy_id = allArgs["id"];
        auto existing = TierStorageManager::Instance().getPolicy(policy_id);
        if (existing.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }

        auto policy = policyFromJson(allArgs.getArgs());

        std::string tier_err;
        if (!validatePolicyByDocRules(policy, tier_err)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, tier_err);
            return;
        }

        if (!TierStorageManager::Instance().updatePolicy(policy)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_UPDATE_FAILED, "Failed to update storage policy");
            return;
        }

        val["data"]["id"] = policy_id;
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/policy/clone
    api_regist("/media/mserver/storage/policy/clone", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id", "name");

        string policy_id = allArgs["id"];
        string new_name  = allArgs["name"];

        auto existing = TierStorageManager::Instance().getPolicy(policy_id);
        if (existing.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }

        auto new_id = TierStorageManager::Instance().clonePolicy(policy_id, new_name);
        if (new_id.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_CLONE_FAILED, "Clone storage policy failed");
            return;
        }

        val["data"]["id"] = new_id;
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/policy/delete
    api_regist("/media/mserver/storage/policy/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string policy_id = allArgs["id"];
        auto existing = TierStorageManager::Instance().getPolicy(policy_id);
        if (existing.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }
        
        int camera_count = 0;
        bool is_default_policy = false;
        if (!TierStorageManager::Instance().deletePolicy(policy_id, camera_count, is_default_policy)) {
            val["data"]["camera_count"] = camera_count;
            val["data"]["is_default_policy"] = is_default_policy;
            if (is_default_policy) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_DEFAULT_CANNOT_DELETE, "Cannot delete system default storage policy");
                return;
            }
            if (camera_count > 0) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_IN_USE, "Policy is applied to " + std::to_string(camera_count) + " cameras");
                return;
            }
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_DELETE_FAILED, "Policy not found or could not be deleted");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
     });

    // ================================================================
    // === Section 3: Policy Assignment APIs (camera level) ============
    // ================================================================

    // POST /media/mserver/storage/policy/assignCamera
    api_regist("/media/mserver/storage/policy/assignCamera", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id", "policy_id");

        string camera_id = allArgs["camera_id"];
        string policy_id = allArgs["policy_id"];
        string reason = allArgs["override_reason"];

        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }

        auto existing = TierStorageManager::Instance().getPolicy(policy_id);
        if (existing.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }

        if (!TierStorageManager::Instance().assignPolicyToCamera(camera_id, policy_id, reason)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_ASSIGN_CAMERA_FAILED, "Assign policy to camera failed");
            return;
        }

        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/policy/assignCameras
    api_regist("/media/mserver/storage/policy/assignCameras", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("policy_id");

        std::string policy_id = allArgs["policy_id"];
        Json::Value camera_ids_arr = allArgs.getArgs()["camera_ids"];
        if (camera_ids_arr.empty() || !camera_ids_arr.isArray()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "camera_ids must be an array");
            return;
        }

        std::vector<std::string> camera_ids;
        std::vector<std::string> non_existed_camera_ids;
        for (const auto &c : camera_ids_arr) {
            std::string camera_id = c.asString();
            auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!device) {
                non_existed_camera_ids.push_back(camera_id);
                continue;  // Skip invalid camera IDs
            }
            camera_ids.push_back(camera_id);
        }

        if (camera_ids.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "No valid camera IDs provided");
            return;
        }

        auto existing = TierStorageManager::Instance().getPolicy(policy_id);
        if (existing.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }

        std::vector<std::string> failed;
        int assigned = TierStorageManager::Instance().assignPolicyToCameras(camera_ids, policy_id, failed);

        Json::Value data;
        data["assigned_count"] = assigned;
        Json::Value failed_arr(Json::arrayValue);
        for (const auto &id : non_existed_camera_ids) {
            failed_arr.append(id);
        }
        for (const auto &id : failed) {
            failed_arr.append(id);
        }
        data["failed_ids"] = failed_arr;
        
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/policy/removeCamera
    api_regist("/media/mserver/storage/policy/removeCamera", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id");

        string camera_id = allArgs["camera_id"];
        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }
        if (!TierStorageManager::Instance().removeCameraOverride(camera_id)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_UNASSIGN_CAMERA_FAILED, "Failed to unassign storage policy from camera");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    // POST /media/mserver/storage/policy/removeCameras
    api_regist("/media/mserver/storage/policy/removeCameras", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);

        Json::Value camera_ids_arr = allArgs.getArgs()["camera_ids"];
        if (camera_ids_arr.empty() || !camera_ids_arr.isArray()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "camera_ids must be an array");
            return;
        }

        std::vector<std::string> camera_ids;
        std::vector<std::string> non_existed_camera_ids;
        for (const auto &c : camera_ids_arr) {
            std::string camera_id = c.asString();
            auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!device) {
                non_existed_camera_ids.push_back(camera_id);
                continue;  // Skip invalid camera IDs
            }
            camera_ids.push_back(camera_id);
        }

        std::vector<std::string> failed;
        int removed = TierStorageManager::Instance().removeCamerasOverride(camera_ids, failed);
        Json::Value data;
        data["removed_count"] = removed;
        Json::Value failed_arr(Json::arrayValue);
        for (const auto &id : non_existed_camera_ids) {
            failed_arr.append(id);
        }
        for (const auto &id : failed) {
            failed_arr.append(id);
        }
        data["failed_ids"] = failed_arr;
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    // GET/POST /media/mserver/storage/camera/effectivePolicy
    api_regist("/media/mserver/storage/camera/effectivePolicy", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id");

        string camera_id = allArgs["camera_id"];
        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }
        auto device_tuple = device->getDeviceTuple();
        auto result = TierStorageManager::Instance().getEffectivePolicy(device_tuple.device_id);
        val["data"] = effectivePolicyToJson(result, device_tuple.name);
        invoker(200, headerOut, val.toStyledString());
    });

    // GET/POST /media/mserver/storage/camera/effectivePolicy/list
    api_regist("/media/mserver/storage/camera/effectivePolicy/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        string search = allArgs["search"];
        string policy_id = allArgs["policy_id"];
        
        std::unordered_map<std::string, std::string> device_map;
        DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
            auto device_tuple = src->getDeviceTuple();
            if (!search.empty() && device_tuple.name.find(search) == std::string::npos)
                return;
            device_map.emplace(device_tuple.device_id, device_tuple.name);
        }, GENERIC_RTSP_CAMERA_SCHEMA);

        Json::Value items(Json::arrayValue);
        for (const auto &it : device_map) {
            const auto &device_id = it.first;
            const auto &device_name = it.second;
            auto result = TierStorageManager::Instance().getEffectivePolicy(device_id);
            if (!policy_id.empty() && result.policy_id != policy_id)
                continue;
            items.append(effectivePolicyToJson(result, device_name));
        }

        Json::Value data;
        data["total"] = static_cast<int>(items.size());
        data["items"] = items;
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    // ================================================================
    // === Section 4: Camera Storage APIs ==============================
    // ================================================================

    // GET/POST /media/mserver/storage/camera/timeline
    api_regist("/media/mserver/storage/camera/timeline", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id", "start_time", "end_time");

        string camera_id      = allArgs["camera_id"];
        int64_t start_time    = allArgs["start_time"];
        int64_t end_time      = allArgs["end_time"];
        bool include_deleted  = allArgs["include_deleted"];

        if (start_time <= 0 || end_time <= 0 || start_time >= end_time) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_TIME_RANGE, "start_time and end_time must be valid");
            return;
        }

        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }
        auto device_tuple = device->getDeviceTuple();

        Json::Value ranges_json(Json::arrayValue);
        auto pool_map = loadPoolMap();
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
            range["restore_required"] = isRestoreRequiredPool(r.pool_id, pool_map);
            range["has_motion"]       = r.has_motion;
            range["has_event"]        = r.has_event;
            ranges_json.append(range);
        }

        auto effective = TierStorageManager::Instance().getEffectivePolicy(camera_id);
        Json::Value data;
        data["camera_id"] = camera_id;
        data["camera_name"] = device_tuple.name;
        data["policy_id"] = effective.policy_id;
        data["ranges"]    = ranges_json;
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    // GET/POST /media/mserver/storage/camera/summary
    api_regist("/media/mserver/storage/camera/summary", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("camera_id");

        string camera_id = allArgs["camera_id"];
        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }
        auto device_tuple = device->getDeviceTuple();

        auto summary = TierStorageManager::Instance().getCameraStorageSummary(camera_id);
        auto effective = TierStorageManager::Instance().getEffectivePolicy(camera_id);

        Json::Value data;
        data["camera_id"]           = summary.camera_id;
        data["camera_name"]         = device_tuple.name;
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

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    // GET/POST /media/mserver/storage/camera/summary/list
    api_regist("/media/mserver/storage/camera/summary/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        string search = allArgs["search"];

        std::unordered_map<std::string, std::string> device_map;
        DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
            auto device_tuple = src->getDeviceTuple();
            if (!search.empty() && device_tuple.name.find(search) == std::string::npos)
                return;
            device_map.emplace(device_tuple.device_id, device_tuple.name);
        }, GENERIC_RTSP_CAMERA_SCHEMA);
        
        Json::Value items(Json::arrayValue);

        for (const auto &it : device_map) {
            const auto &camera_id = it.first;
            const auto &camera_name = it.second;

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
            items.append(data);
        }

        val["data"] = items;
        invoker(200, headerOut, val.toStyledString());
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

        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }

        auto pool_map = loadPoolMap();
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
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_SEGMENTS_NOT_FOUND, "No segments found for the specified time range");
            return;
        }

        std::string tier = matched.front().tier;
        std::string resolved_stream_id = stream_id.empty() ? matched.front().stream_id : stream_id;
        bool restore_required = false;
        bool has_expired = false;
        bool has_deleted = false;
        bool has_missing = false;
        for (const auto &r : matched) {
            if (r.status == segmentStatusToString(SegmentStatus::EXPIRED)) has_expired = true;
            if (r.status == segmentStatusToString(SegmentStatus::DELETED)) has_deleted = true;
            if (r.status == segmentStatusToString(SegmentStatus::MISSING)) has_missing = true;
            if (isRestoreRequiredPool(r.pool_id, pool_map)) {
                restore_required = true;
                tier = r.tier;
                if (resolved_stream_id.empty())
                    resolved_stream_id = r.stream_id;
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
            RETURN_API_RESPONSE(has_expired ? ApiErrCode::CODE_STORAGE_SEGMENTS_EXPIRED
                                            : ApiErrCode::CODE_STORAGE_SEGMENTS_NOT_FOUND,
                                has_expired ? "Some segments have expired"
                                            : "Some segments are not available");
            return;
        }

        RestoreJobImp restore_imp;
        auto jobs = restore_imp.queryOverlapping(camera_id, start_time, end_time, 20);
        const RestoreJob *covering_done = nullptr;
        const RestoreJob *active_job = nullptr;
        const RestoreJob *latest_job = nullptr;
        for (const auto &j : jobs) {
            if (!latest_job)
                latest_job = &j;
            if (j.start_time <= start_time && j.end_time >= end_time &&
                j.status == jobStatusToString(JobStatus::DONE)) {
                covering_done = &j;
                break;
            }
            if (!active_job &&
                (j.status == jobStatusToString(JobStatus::PENDING) ||
                 j.status == jobStatusToString(JobStatus::RUNNING))) {
                active_job = &j;
            }
        }

        if (restore_required && !covering_done) {
            Json::Value data;
            data["status"] = active_job ? "RESTORING" : "RESTORE_REQUIRED";
            data["tier"] = tier.empty() ? tierTypeToString(ColdTier) : tier;
            data["restore_required"] = true;
            data["job_created"] = (active_job  != nullptr || latest_job != nullptr);
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
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_RESTORE_REQUIRED,
                                active_job ? "Restore job is running"
                                           : "Recording is in object storage and must be restored before playback");
            return;
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
        GET_CONFIG(int, restore_ttl_seconds, Storage::kRestoreTTLSeconds);
        data["expires_at"] = static_cast<Json::Int64>(time(nullptr) + restore_ttl_seconds);
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/restoreJob/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE, PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("camera_id", "start_time", "end_time");

        string camera_id = allArgs["camera_id"];
        int64_t start_time = allArgs["start_time"];
        int64_t end_time = allArgs["end_time"];
        string target_tier = allArgs["target_tier"];
        string reason = allArgs["reason"];

        if (target_tier.empty()) {
            target_tier = "HOT";
        }
        if (start_time <= 0 || end_time <= 0 || start_time >= end_time) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_TIME_RANGE, "start_time and end_time must be valid");
            return;
        }

        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }

        int64_t total_bytes = 0;
        std::string source_tier = "COLD";
        SegmentTierRangeImp range_imp;
        bool found_cold = false;
        for (const auto &r : range_imp.queryByCamera(camera_id, start_time, end_time)) {
            if (r.status == segmentStatusToString(SegmentStatus::DELETED) ||
                r.status == segmentStatusToString(SegmentStatus::EXPIRED) ||
                r.status == segmentStatusToString(SegmentStatus::MISSING)) {
                continue;
            }
            total_bytes += std::max<int64_t>(0, r.size_bytes);
            if (!found_cold || r.tier == tierTypeToString(ColdTier)) {
                source_tier = r.tier;
                found_cold = r.tier == tierTypeToString(ColdTier);
            }
        }

        RestoreJob job;
        job.job_id = StrUUID::make_guid(8, "rj");
        job.camera_id = camera_id;
        job.source_tier = source_tier;
        job.target_tier = target_tier.empty() ? "HOT" : target_tier;
        job.status = "PENDING";
        job.start_time = start_time;
        job.end_time = end_time;
        job.total_bytes = total_bytes;
        job.processed_bytes = 0;
        job.reason = Optional<std::string>(reason);
        job.created_at = static_cast<int64_t>(time(nullptr));
        job.updated_at = job.created_at;

        RestoreJobImp imp;
        if (!imp.add(job)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_RESTORE_JOB_CREATION_FAILED, "Failed to create restore job");
            return;
        }

        Json::Value data;
        data["job_id"] = job.job_id;
        data["status"] = job.status;
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/restoreJob/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE, PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("job_id");

        string job_id = allArgs["job_id"];

        auto jobs = TierStorageManager::Instance().getRestoreJob(job_id);
        if (jobs.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_RESTORE_JOB_NOT_FOUND, "Restore job not found");
            return;
        }
        val["data"] = restoreJobToDocJson(jobs.front());
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/restoreJob/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE, PLAYBACK_PERMISSION_CODE);

        std::string camera_id = allArgs["camera_id"];
        std::string status = allArgs["status"];
        int64_t from_time = allArgs["from_time"];
        int64_t to_time = allArgs["to_time"];
        int page = allArgs["page"].empty() ? 0 : allArgs["page"].as<int>();
        int size = allArgs["size"].empty() ? 20 : allArgs["size"].as<int>();
        if (size <= 0 || size > 100) size = 20;

        auto jobs = TierStorageManager::Instance().listRestoreJobs(camera_id, status, from_time, to_time, page, size);
        Json::Value items(Json::arrayValue);
        for (const auto &j : jobs)
            items.append(restoreJobToDocJson(j, true));

        auto total_count = TierStorageManager::Instance().countRestoreJobs(camera_id, status, from_time, to_time);

        Json::Value data;
        data["items"] = items;
        data["page"] = page;
        data["size"] = size;
        data["total"] = total_count;

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    // ================================================================
    // === Section 6: Tiering Job APIs ================================
    // ================================================================

    api_regist("/media/mserver/storage/tieringJob/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        std::string status = allArgs["status"];
        std::string camera_id = allArgs["camera_id"];
        std::string source_tier = allArgs["source_tier"];
        std::string target_tier = allArgs["target_tier"];
        int64_t from_time = allArgs["from_time"];
        int64_t to_time = allArgs["to_time"];
        int page = allArgs["page"].empty() ? 0 : allArgs["page"].as<int>();
        int size = allArgs["size"].empty() ? 20 : allArgs["size"].as<int>();
        if (size <= 0 || size > 100) size = 20;

        Json::Value items(Json::arrayValue);
        auto jobs = TierStorageManager::Instance().listTieringJobs(
            camera_id, status, source_tier, target_tier, from_time, to_time, page, size);
        for (const auto &j : jobs)
            items.append(tieringJobToDocJson(j));

        auto total_count = TierStorageManager::Instance().countTieringJobs(
            camera_id, status, from_time, to_time);

        Json::Value data;
        data["items"] = items;
        data["page"] = page;
        data["size"] = size;
        data["total"] = total_count;

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/tieringJob/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("job_id");

        string job_id = allArgs["job_id"];
        auto jobs = TierStorageManager::Instance().getTieringJob(job_id);
        if (jobs.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_TIERING_JOB_NOT_FOUND, "Tiering job not found");
            return;
        }
        val["data"] = tieringJobToDocJson(jobs.front(), true);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/tieringJob/retry", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("job_id");
        string job_id = allArgs["job_id"];

        auto jobs = TierStorageManager::Instance().getTieringJob(job_id);
        if (jobs.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_TIERING_JOB_NOT_FOUND, "Tiering job not found");
            return;
        }
        if (!TierStorageManager::Instance().retryTieringJob(job_id)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_TIERING_JOB_UPDATE_FAILED, "Failed to retry tiering job");
            return;
        }   

        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/tieringJob/cancel", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("job_id");

        string job_id = allArgs["job_id"];
        if (!TierStorageManager::Instance().cancelTieringJob(job_id)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_TIERING_JOB_CANCEL_FAILED, "Failed to cancel tiering job");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    // ================================================================
    // === Section 7: Storage Dashboard APIs ==========================
    // ================================================================
    api_regist("/media/mserver/storage/dashboard/summary", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        val["data"] = dashboardSummaryToDocJson();
    });

    // ================================================================
    // === Section 8: Alert APIs ======================================
    // ================================================================

    api_regist("/media/mserver/storage/alert/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        std::string level = allArgs["level"];
        int acknowledged = allArgs["level"].empty() ? -1 : std::stoi(allArgs["acknowledged"]);
        int page = allArgs["page"].empty() ? 0 : std::stoi(allArgs["page"]);
        int size = allArgs["size"].empty() ? 20 : std::stoi(allArgs["size"]);
        if (size <= 0 || size > 100) size = 20;

        StorageAlertImp imp;
        Json::Value items(Json::arrayValue);
        auto pool_map = loadPoolMap();
        for (const auto &a : imp.query(level, acknowledged, page, size))
            items.append(alertToDocJson(a, pool_map));

        Json::Value data;
        data["items"] = items;
        data["page"] = page;
        data["size"] = size;
        data["total"] = imp.countQuery(level, acknowledged);

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/alert/ack", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("alert_id");

        string alert_id = allArgs["alert_id"];
        StorageAlertImp imp;
        if (!imp.acknowledge(alert_id)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_ALERT_NOT_FOUND, "Alert not found or could not be acknowledged");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    // ================================================================
    // === Section 9: Expired / Delete Approval APIs ==================
    // ================================================================

    api_regist("/media/mserver/storage/expiredSegment/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        std::string camera_id = allArgs["camera_id"];
        int64_t from_time = allArgs["from_time"].empty() ? 0 : std::stoll(allArgs["from_time"]);
        int64_t to_time = allArgs["to_time"].empty() ? 0 : std::stoll(allArgs["to_time"]);
        int page = allArgs["page"].empty() ? 0 : std::stoi(allArgs["page"]);
        int size = allArgs["size"].empty() ? 20 : std::stoi(allArgs["size"]);
        if (size <= 0 || size > 100) size = 20;

        SegmentTierRangeImp imp;
        Json::Value items(Json::arrayValue);
        for (const auto &r : imp.findExpired(camera_id, from_time, to_time, page, size))
            items.append(expiredSegmentToDocJson(r));

        Json::Value data;
        data["items"] = items;
        data["page"] = page;
        data["size"] = size;
        data["total"] = imp.countExpired(camera_id, from_time, to_time);

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/expiredSegment/approve", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);

        Json::Value range_ids = allArgs.getArgs()["range_ids"];
        if (range_ids.empty() || !range_ids.isArray()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "range_ids must be an array");
            return;
        }

        SegmentTierRangeImp imp;
        int approved = 0;
        for (const auto &idv : range_ids) {
            std::string range_id = idv.asString();
            if (range_id.empty())
                continue;
            if (imp.updateStatusByRangeId(range_id, segmentStatusToString(SegmentStatus::DELETED)))
                ++approved;
        }

        Json::Value data;
        data["approved_count"] = approved;
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/expiredSegment/extend", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(MODIFY_MSERVER_PERMISSION_CODE);

        Json::Value range_ids = allArgs.getArgs()["range_ids"];
        if (range_ids.empty())
            range_ids = allArgs.getArgs()["segment_ids"];
        int64_t extend_until = allArgs["extend_until"].empty() ? 0 : std::stoll(allArgs["extend_until"]);

        if (range_ids.empty() || !range_ids.isArray()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "range_ids must be an array");
            return;
        }

        if (extend_until <= 0) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "extend_until is required");
            return;
        }

        SegmentTierRangeImp imp;
        int extended = 0;
        for (const auto &idv : range_ids) {
            std::string range_id = idv.asString();
            if (range_id.empty())
                continue;
            (void)extend_until;
            if (imp.updateStatusByRangeId(range_id, segmentStatusToString(SegmentStatus::AVAILABLE)))
                ++extended;
        }

        Json::Value data;
        data["extended_count"] = extended;
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    // ================================================================
    // === Section 10: Protected Video APIs ===========================
    // ================================================================

    api_regist("/media/mserver/storage/protected/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE, MODIFY_MSERVER_PERMISSION_CODE);

        std::string camera_id = allArgs["camera_id"];
        int64_t start_time = allArgs["start_time"].empty() ? 0 : std::stoll(allArgs["start_time"]);
        int64_t end_time = allArgs["end_time"].empty() ? 0 : std::stoll(allArgs["end_time"]);
        std::string type = allArgs["type"].empty() ? "PROTECTED" : allArgs["type"];
        std::string reason = allArgs["reason"];

        if (start_time <= 0 || end_time <= 0 || start_time >= end_time) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_TIME_RANGE, "start_time and end_time must be valid");
            return;
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
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_SEGMENT_PROTECTED_CREATE_FAILED, "Failed to create protected video");
            return;
        }

        Json::Value data;
        data["protected_id"] = video.protected_id;

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/protected/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE, MODIFY_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("protected_id");

        std::string protected_id = allArgs["protected_id"];

        ProtectedVideoImp imp;
        if (!imp.remove(protected_id)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_SEGMENT_PROTECTED_NOT_FOUND, "Failed to delete protected video");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/protected/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE, READ_MSERVER_PERMISSION_CODE);

        std::string camera_id = allArgs["camera_id"];
        std::string type = allArgs["type"];
        int64_t from_time = allArgs["from_time"].empty() ? 0 : std::stoll(allArgs["from_time"]);
        int64_t to_time = allArgs["to_time"].empty() ? 0 : std::stoll(allArgs["to_time"]);
        int page = allArgs["page"].empty() ? 0 : std::stoi(allArgs["page"]);
        int size = allArgs["size"].empty() ? 20 : std::stoi(allArgs["size"]);
        if (size <= 0 || size > 100) size = 20;

        ProtectedVideoImp imp;
        Json::Value items(Json::arrayValue);
        for (const auto &p : imp.query(camera_id, type, from_time, to_time, page, size))
            items.append(protectedVideoToDocJson(p));

        Json::Value data;
        data["items"] = items;
        data["page"] = page;
        data["size"] = size;
        data["total"] = imp.countQuery(camera_id, type, from_time, to_time);

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/dashboard/detail", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        val["data"] = TierStorageManager::Instance().getDashboardDetail();
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/pool/options", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        
        Json::Value data;
        data["poolsTypeSupport"] = Json::objectValue;
        data["poolsTypeSupport"]["HOT"] = poolTypeSupport(TierType::HotTier);
        data["poolsTypeSupport"]["WARM"] = poolTypeSupport(TierType::WarmTier);
        data["poolsTypeSupport"]["COLD"] = poolTypeSupport(TierType::ColdTier);

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/storage/mountpoint/available", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        string storage_types = allArgs["include"];
        auto available_mount_points = TierStorageManager::Instance().getAvailableMountPoints(storage_types);

        Json::Value data;
        data["mount_point"] = Json::arrayValue;
        for (const auto &mp : available_mount_points) {
            data["mount_point"].append(mp.toJson());
        }
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    InfoL << "Storage tiering APIs registered";
}

} // namespace managerkit
