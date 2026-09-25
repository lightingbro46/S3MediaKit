#include "TierStorageBase.h"

#ifdef ENABLE_TIER_STORAGE

#include "StorageTier.h"
#include "TierEncryptionManager.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace managerkit {

namespace {

const char *kMaskedCredential = "***********";

Optional<std::string> optionalString(const Json::Value &body,
                                     const char *key,
                                     bool credential = false) {
    if (!body.isMember(key) || body[key].isNull()) {
        return Optional<std::string>();
    }
    const std::string value = body[key].asString();
    if (value.empty() || (credential && value == kMaskedCredential)) {
        return Optional<std::string>();
    }
    return Optional<std::string>(value);
}

void applyOptionalString(Optional<std::string> &target,
                         const Json::Value &body,
                         const char *key,
                         bool credential = false) {
    Optional<std::string> value = optionalString(body, key, credential);
    if (value.has_value()) {
        target = value;
    }
}

// Absent or null yields no value; a malformed or fractional number yields -1
// so validation rejects it instead of silently dropping the quota.
Optional<int64_t> optionalBytes(const Json::Value &value) {
    if (value.isNull())
        return Optional<int64_t>();
    if (value.isIntegral())
        return Optional<int64_t>(value.asInt64());
    if (value.isString() && !value.asString().empty()) {
        const std::string text = value.asString();
        char *end = nullptr;
        errno = 0;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0 && end && *end == '\0')
            return Optional<int64_t>(static_cast<int64_t>(parsed));
    }
    return Optional<int64_t>(-1);
}

std::string capacitySourceFromJson(const Json::Value &value) {
    std::string source = value.asString();
    std::transform(source.begin(), source.end(), source.begin(), ::toupper);
    return source.empty() ? std::string(kPoolCapacitySourceMetrics) : source;
}

} // namespace

StoragePool TierStorageBase::poolFromJson(const Json::Value &body) {
    StoragePool pool;
    pool.id = body.get("id", "").asString();
    pool.name = body.get("name", "").asString();
    pool.type = body.get("type", "").asString();
    pool.tier = body.get("tier", "").asString();
    pool.endpoint = optionalString(body, "endpoint");
    pool.bucket = optionalString(body, "bucket");
    pool.base_path = optionalString(body, "base_path");
    pool.access_key = optionalString(body, "access_key", true);
    pool.secret_key_enc = optionalString(body, "secret_key", true);
    pool.mount_path = optionalString(body, "mount_path");
    pool.network_path = optionalString(body, "network_path");
    pool.enabled = body.get("enabled", true).asBool() ? 1 : 0;
    pool.health_check_enabled = body.get("health_check_enabled", true).asBool() ? 1 : 0;
    pool.high_watermark_percent = body.get("high_watermark_percent", 85).asInt();
    pool.critical_watermark_percent = body.get("critical_watermark_percent", 90).asInt();
    if (body.isMember("capacity_quota_bytes"))
        pool.capacity_quota_bytes = optionalBytes(body["capacity_quota_bytes"]);
    if (body.isMember("capacity_source"))
        pool.capacity_source = capacitySourceFromJson(body["capacity_source"]);
    return pool;
}

void TierStorageBase::applyPoolPatch(StoragePool &pool, const Json::Value &body) {
    if (body.isMember("name") && !body["name"].asString().empty())
        pool.name = body["name"].asString();
    if (body.isMember("type") && !body["type"].asString().empty())
        pool.type = body["type"].asString();
    if (body.isMember("tier") && !body["tier"].asString().empty())
        pool.tier = body["tier"].asString();
    if (body.isMember("enabled"))
        pool.enabled = body["enabled"].asBool() ? 1 : 0;
    if (body.isMember("health_check_enabled"))
        pool.health_check_enabled = body["health_check_enabled"].asBool() ? 1 : 0;
    if (body.isMember("high_watermark_percent"))
        pool.high_watermark_percent = body["high_watermark_percent"].asInt();
    if (body.isMember("critical_watermark_percent"))
        pool.critical_watermark_percent = body["critical_watermark_percent"].asInt();
    // null clears the quota; an absent key keeps it.
    if (body.isMember("capacity_quota_bytes"))
        pool.capacity_quota_bytes = optionalBytes(body["capacity_quota_bytes"]);
    if (body.isMember("capacity_source"))
        pool.capacity_source = capacitySourceFromJson(body["capacity_source"]);

    applyOptionalString(pool.endpoint, body, "endpoint");
    applyOptionalString(pool.bucket, body, "bucket");
    applyOptionalString(pool.base_path, body, "base_path");
    applyOptionalString(pool.access_key, body, "access_key", true);
    applyOptionalString(pool.secret_key_enc, body, "secret_key", true);
    applyOptionalString(pool.mount_path, body, "mount_path");
    applyOptionalString(pool.network_path, body, "network_path");
}

bool TierStorageBase::validatePoolByType(const StoragePool &pool, std::string &error) {
#ifndef ENABLE_AWS_SDK
    // Say why, not just "not supported": the same request works on a build with object storage.
    if (poolTypeIsObjectStorage(pool.type)) {
        error = pool.type + " pool requires object storage, which this build does not include (ENABLE_AWS_SDK=OFF)";
        return false;
    }
#endif
    Json::Value support = poolTypeSupport(tierTypeFromString(pool.tier));
    if (support.isMember(pool.type) && !support[pool.type].asBool()) {
        error = pool.type + " pool is not supported for " + pool.tier + " tier";
        return false;
    }
    if (pool.type == "LOCAL_DISK") {
        if (!pool.mount_path.has_value() || pool.mount_path.value().empty()) {
            error = "mount_path is required for LOCAL_DISK";
            return false;
        }
    } else if (pool.type == "NAS") {
        const bool has_mount = pool.mount_path.has_value() && !pool.mount_path.value().empty();
        const bool has_network = pool.network_path.has_value() && !pool.network_path.value().empty();
        if (!has_mount && !has_network) {
            error = "mount_path or network_path is required for NAS";
            return false;
        }
    } else if (pool.type == "MINIO" || pool.type == "S3") {
        if (!pool.endpoint.has_value() || pool.endpoint.value().empty() ||
            !pool.bucket.has_value() || pool.bucket.value().empty() ||
            !pool.base_path.has_value() || pool.base_path.value().empty() ||
            !pool.access_key.has_value() || pool.access_key.value().empty() ||
            !pool.secret_key_enc.has_value() || pool.secret_key_enc.value().empty()) {
            error = "endpoint, bucket, base_path, access_key and secret_key are required for MINIO/S3";
            return false;
        }
    }
    if (pool.capacity_quota_bytes.has_value() && pool.capacity_quota_bytes.value() <= 0) {
        error = "capacity_quota_bytes must be a positive integer";
        return false;
    }
    if (pool.capacity_source == kPoolCapacitySourceQuota) {
        if (!poolTypeIsObjectStorage(pool.type)) {
            error = "capacity_source QUOTA is only supported for object storage pools";
            return false;
        }
        if (!pool.capacity_quota_bytes.has_value()) {
            error = "capacity_quota_bytes is required when capacity_source is QUOTA";
            return false;
        }
    } else if (pool.capacity_source != kPoolCapacitySourceMetrics) {
        error = "capacity_source must be METRICS or QUOTA";
        return false;
    }
    return true;
}

StoragePolicy TierStorageBase::policyFromJson(const Json::Value &body) {
    StoragePolicy policy;
    policy.id = body.get("id", "").asString();
    policy.name = body.get("name", "").asString();
    if (body.isMember("description") && !body["description"].isNull())
        policy.description = Optional<std::string>(body["description"].asString());
    policy.enabled = body.get("enabled", true).asBool() ? 1 : 0;
    policy.total_retention_days = body.get("total_retention_days", 30).asInt();
    policy.allow_camera_override = body.get("allow_camera_override", true).asBool() ? 1 : 0;
    policy.protect_event_video = body.get("protect_event_video", false).asBool() ? 1 : 0;

    Json::FastWriter writer;
    policy.tiers_json = body.isMember("tiers") && body["tiers"].isArray()
        ? writer.write(body["tiers"]) : "[]";
    policy.delete_policy_json = body.isMember("delete_policy") && body["delete_policy"].isObject()
        ? writer.write(body["delete_policy"]) : "{}";
    policy.advanced_rules_json = body.isMember("advanced_rules") && body["advanced_rules"].isObject()
        ? writer.write(body["advanced_rules"]) : "{}";
    return policy;
}

bool TierStorageBase::validatePolicyTierMoveChain(
        const std::vector<PolicyTierConfig> &tiers,
        std::string &error) {
    std::vector<PolicyTierConfig> sorted;
    for (const auto &tier : tiers) {
        if (tier.enabled)
            sorted.push_back(tier);
    }
    std::sort(sorted.begin(), sorted.end(), [](const PolicyTierConfig &left,
                                                const PolicyTierConfig &right) {
        return tierTypeFromString(left.tier) < tierTypeFromString(right.tier);
    });
    if (sorted.empty() || sorted.front().tier != "HOT") {
        error = "HOT tier must be enabled";
        return false;
    }
    for (size_t index = 0; index + 1 < sorted.size(); ++index) {
        if (sorted[index].overflow_action != "MOVE_TO_NEXT_TIER") {
            error = sorted[index + 1].tier + " tier can only be enabled when " +
                    sorted[index].tier + " overflow_action is MOVE_TO_NEXT_TIER";
            return false;
        }
    }
    // With no enabled tier after it, MOVE_TO_NEXT_TIER would keep data forever.
    if (sorted.back().overflow_action == "MOVE_TO_NEXT_TIER") {
        error = sorted.back().tier +
                " is the last enabled tier and cannot use overflow_action MOVE_TO_NEXT_TIER";
        return false;
    }
    return true;
}

bool TierStorageBase::validatePolicyTiers(const std::vector<PolicyTierConfig> &tiers,
                                          std::string &error) {
    if (!validatePolicyTierMoveChain(tiers, error))
        return false;

    std::vector<PolicyTierConfig> sorted;
    for (const auto &tier : tiers) {
        if (tier.enabled)
            sorted.push_back(tier);
    }
    std::sort(sorted.begin(), sorted.end(), [](const PolicyTierConfig &left,
                                                const PolicyTierConfig &right) {
        return tierTypeFromString(left.tier) < tierTypeFromString(right.tier);
    });

    int previous_days = -1;
    std::string previous_tier;
    for (const auto &tier : sorted) {
        if (previous_days >= 0 && previous_days >= tier.retain_until_days) {
            error = previous_tier + ".retain_until_days must be less than " +
                    tier.tier + ".retain_until_days";
            return false;
        }
        if (tier.overflow_action == "DELETE_OLDEST" && tier.retain_until_days <= 0) {
            error = tier.tier + ".retain_until_days must be greater than 0 for DELETE_OLDEST";
            return false;
        }
        if (tier.high_watermark_percent >= tier.critical_watermark_percent) {
            error = "high_watermark_percent must be less than critical_watermark_percent";
            return false;
        }
        previous_days = tier.retain_until_days;
        previous_tier = tier.tier;
    }
    return true;
}

bool TierStorageBase::validateStoragePolicy(const StoragePolicy &policy,
                                            const StoragePoolMap &pool_map,
                                            std::string &error) {
    const auto tiers = policy.parsedTiers();
    if (!validatePolicyTiers(tiers, error))
        return false;

    int max_retain_days = 0;
    for (const auto &tier : tiers) {
        if (!tier.enabled)
            continue;
        if (tier.data_mode != "FULL_VIDEO" || tier.priority != "NORMAL" ||
            tier.overflow_action == "STOP_RECORDING_AND_ALERT") {
            error = "Only FULL_VIDEO, NORMAL priority and executable overflow actions are supported";
            return false;
        }
        max_retain_days = std::max(max_retain_days, tier.retain_until_days);
        if (tier.pool_id.empty()) {
            error = tier.tier + ".pool_id is required";
            return false;
        }
        auto pool_it = pool_map.find(tier.pool_id);
        if (pool_it == pool_map.end() || pool_it->second.enabled == 0) {
            error = "pool_id must exist and be enabled: " + tier.pool_id;
            return false;
        }
#ifndef ENABLE_AWS_SDK
        if (poolTypeIsObjectStorage(pool_it->second.type)) {
            error = "Object storage requires ENABLE_AWS_SDK";
            return false;
        }
#endif
        if (pool_it->second.tier != tier.tier) {
            error = "pool_id " + tier.pool_id + " belongs to " + pool_it->second.tier +
                    " tier, expected " + tier.tier;
            return false;
        }
        const auto support = poolTypeSupport(tierTypeFromString(tier.tier));
        if (support.isMember(pool_it->second.type) && !support[pool_it->second.type].asBool()) {
            error = pool_it->second.type + " pool is not supported for " + tier.tier + " tier";
            return false;
        }
        if (tier.critical_watermark_percent > 95) {
            error = "critical_watermark_percent must be <= 95";
            return false;
        }
    }

    if (policy.parsedDeletePolicy().delete_after_days < max_retain_days) {
        error = "delete_after_days must be greater than or equal to the largest retain_until_days";
        return false;
    }
    return true;
}

TierStorageBase::StoragePoolMap TierStorageBase::loadPoolMap() {
    StoragePoolImp repository;
    StoragePoolMap pools;
    for (const auto &pool : repository.queryAll())
        pools[pool.id] = pool;
    return pools;
}

bool TierStorageBase::isRestoreRequiredPool(const std::string &pool_id,
                                            const StoragePoolMap &pool_map) {
    auto pool_it = pool_map.find(pool_id);
    return pool_it != pool_map.end() && poolTypeIsObjectStorage(pool_it->second.type);
}

static const char *encryptionJobStatusToString(EncryptionJobStatus st) {
    switch (st) {
        case EncryptionJobStatus::kIdle:      return "IDLE";
        case EncryptionJobStatus::kRunning:   return "RUNNING";
        case EncryptionJobStatus::kCompleted: return "COMPLETED";
        case EncryptionJobStatus::kFailed:    return "FAILED";
        default:                              return "UNKNOWN";
    }
}

Json::Value TierStorageBase::encryptionStatusToJson(const StoragePool &p) {
    Json::Value enc;
    auto &mgr = TierEncryptionManager::Instance();

    const bool hot = (p.tier == tierTypeToString(HotTier));
    enc["supported"] = hot;

    if (!hot || !mgr.tierExists(p.id)) {
        enc["configured"] = false;
        enc["mode"] = static_cast<int>(RecordEncryptionMode::kNone);
        enc["encryptOldVideo"] = false;
        return enc;
    }

    auto cfg = mgr.getTierConfig(p.id);
    enc["configured"] = true;
    enc["mode"] = static_cast<int>(cfg.mode);
    enc["encryptOldVideo"] = cfg.encrypt_old_video;
    enc["updated_at"] = static_cast<Json::Int64>(cfg.updated_at);
    enc["key_history_count"] = static_cast<Json::UInt>(cfg.time_ranges.size());

    auto prog = mgr.getReencryptProgress(p.id);
    Json::Value job;
    job["status"] = encryptionJobStatusToString(prog.status);
    job["started_at"] = static_cast<Json::Int64>(prog.started_at);
    job["finished_at"] = static_cast<Json::Int64>(prog.finished_at);
    job["total_files"] = static_cast<Json::UInt>(prog.total_files);
    job["processed_files"] = static_cast<Json::UInt>(prog.processed_files);
    job["failed_files"] = static_cast<Json::UInt>(prog.failed_files);
    job["skipped_files"] = static_cast<Json::UInt>(prog.skipped_files);
    job["restored_files"] = static_cast<Json::UInt>(prog.restored_files);
    job["current_file"] = prog.current_file;
    job["error_msg"] = prog.error_msg;
    job["percent"] = prog.total_files > 0
        ? static_cast<int>((prog.processed_files * 100) / prog.total_files)
        : (prog.status == EncryptionJobStatus::kCompleted ? 100 : 0);
    enc["reencrypt_job"] = job;
    enc["job_running_any_tier"] = mgr.isAnyJobRunning();
    enc["running_job_tier_id"] = mgr.getRunningJobTierId();
    return enc;
}

Json::Value TierStorageBase::poolSummaryToJson(const StoragePool &pool) {
    Json::Value value;
    const int64_t free_bytes = pool.total_bytes > pool.used_bytes
        ? pool.total_bytes - pool.used_bytes : 0;
    const int used_percent = pool.total_bytes > 0
        ? static_cast<int>((pool.used_bytes * 100) / pool.total_bytes)
        : static_cast<int>(pool.usage_pct);
    value["id"] = pool.id;
    value["name"] = pool.name;
    value["type"] = pool.type;
    value["tier"] = pool.tier;
    value["status"] = pool.health_status.empty() ? "OK" : pool.health_status;
    value["total_bytes"] = static_cast<Json::Int64>(pool.total_bytes);
    value["used_bytes"] = static_cast<Json::Int64>(pool.used_bytes);
    value["free_bytes"] = static_cast<Json::Int64>(free_bytes);
    value["used_percent"] = used_percent;
    value["write_mbps"] = 0;
    value["read_mbps"] = 0;
    value["camera_count"] = 0;
    value["enabled"] = (pool.enabled != 0);
    value["last_health_check"] = static_cast<Json::Int64>(pool.last_health_check);
    value["capacity_available"] = pool.capacity_available;
    value["capacity_status"] = pool.capacity_status;
    value["capacity_message"] = pool.capacity_message;
    return value;
}

Json::Value TierStorageBase::poolDetailToJson(const StoragePool &pool) {
    Json::Value value = poolSummaryToJson(pool);
    value["health_status"] = pool.health_status.empty() ? "OK" : pool.health_status;
    value["usage_pct"] = pool.usage_pct;
    value["access_key"] = pool.access_key.has_value() && !pool.access_key.value().empty()
        ? kMaskedCredential : "";
    value["secret_key"] = pool.secret_key_enc.has_value() && !pool.secret_key_enc.value().empty()
        ? kMaskedCredential : "";
    value["base_path"] = pool.base_path.value_or("");
    value["bucket"] = pool.bucket.value_or("");
    value["created_at"] = static_cast<Json::Int64>(pool.created_at);
    value["critical_watermark_percent"] = pool.critical_watermark_percent;
    value["endpoint"] = pool.endpoint.value_or("");
    value["health_check_enabled"] = (pool.health_check_enabled != 0);
    value["high_watermark_percent"] = pool.high_watermark_percent;
    value["mount_path"] = pool.mount_path.value_or("");
    value["network_path"] = pool.network_path.value_or("");
    value["updated_at"] = static_cast<Json::Int64>(pool.updated_at);
    value["capacity_source"] = pool.capacity_source;
    if (pool.capacity_quota_bytes.has_value())
        value["capacity_quota_bytes"] = static_cast<Json::Int64>(pool.capacity_quota_bytes.value());
    else
        value["capacity_quota_bytes"] = Json::nullValue;
    value["used_percent"] = pool.total_bytes > 0
        ? static_cast<double>(pool.used_bytes) * 100.0 / static_cast<double>(pool.total_bytes)
        : static_cast<double>(pool.usage_pct);
    // Zero unless the caller ran fillPoolEncryptionUsage() first (pool/detail does).
    value["encrypted_bytes"]   = static_cast<Json::Int64>(pool.encrypted_bytes);
    value["unencrypted_bytes"] = static_cast<Json::Int64>(pool.unencrypted_bytes);
    value["encrypted_pct"]     = pool.encrypted_pct;
    value["unencrypted_pct"]   = pool.unencrypted_pct;
    value["encryption"] = encryptionStatusToJson(pool);
    return value;
}

Json::Value TierStorageBase::policySummaryToJson(const StoragePolicy &policy,
                                                 int applied_camera_count) {
    Json::Value value = policy.toJson();
    value.removeMember("tiers");
    value.removeMember("delete_policy");
    value.removeMember("advanced_rules");
    value["hot_retain_until_days"] = 0;
    value["warm_retain_until_days"] = 0;
    value["cold_retain_until_days"] = 0;
    for (const auto &tier : policy.parsedTiers()) {
        if (tier.tier == "HOT") value["hot_retain_until_days"] = tier.retain_until_days;
        if (tier.tier == "WARM") value["warm_retain_until_days"] = tier.retain_until_days;
        if (tier.tier == "COLD") value["cold_retain_until_days"] = tier.retain_until_days;
    }
    value["delete_after_days"] = policy.parsedDeletePolicy().delete_after_days;
    value["applied_camera_count"] = applied_camera_count;
    return value;
}

Json::Value TierStorageBase::policyDetailToJson(const StoragePolicy &policy,
                                                const StoragePoolMap &pool_map,
                                                bool include_summary_fields) {
    Json::Value value = include_summary_fields
        ? policySummaryToJson(policy, 0) : policy.toJson();
    if (!include_summary_fields) {
        value.removeMember("created_at");
        value.removeMember("updated_at");
    }
    Json::Value tiers(Json::arrayValue);
    for (const auto &tier : policy.parsedTiers()) {
        Json::Value tier_value = tier.toJson();
        auto pool_it = pool_map.find(tier.pool_id);
        tier_value["pool_name"] = pool_it == pool_map.end() ? "" : pool_it->second.name;
        tiers.append(tier_value);
    }
    value["tiers"] = tiers;
    value["delete_policy"] = policy.parsedDeletePolicy().toJson();
    value["advanced_rules"] = policy.parsedAdvancedRules().toJson();
    return value;
}

Json::Value TierStorageBase::effectivePolicyToJson(const EffectivePolicyResult &result,
                                                   const std::string &camera_name) {
    Json::Value value = result.toJson();
    value["camera_name"] = camera_name;
    return value;
}

namespace {

int progressPercent(int64_t processed, int64_t total, const std::string &status) {
    if (status == "DONE") return 100;
    if (total <= 0) return processed > 0 ? 100 : 0;
    return std::max(0, std::min(100, static_cast<int>((processed * 100) / total)));
}

} // namespace

Json::Value TierStorageBase::restoreJobToJson(const RestoreJob &job,
                                              const std::string &camera_name,
                                              bool list_view) {
    Json::Value value = job.toJson();
    value["progress_percent"] = progressPercent(job.processed_bytes, job.total_bytes, job.status);
    value["playback_ready"] = (job.status == "DONE");
    value.removeMember("reason");
    value.removeMember("error_message");
    if (list_view) {
        value["camera_name"] = camera_name;
        value.removeMember("type");
        value.removeMember("start_time");
        value.removeMember("end_time");
        value.removeMember("processed_bytes");
        value.removeMember("updated_at");
        value.removeMember("reason");
        value.removeMember("error_message");
    } else {
        value["type"] = "RESTORE";
    }
    return value;
}

Json::Value TierStorageBase::restoreJobSnapshotToJson(const RestoreJobSnapshot &snapshot,
                                                      const std::string &camera_name) {
    Json::Value value = restoreJobToJson(snapshot.job, camera_name, false);
    value["playback_status"] = snapshot.playback_status;
    value["playback_ready"] = snapshot.playback_ready;
    value["total_segment_count"] = snapshot.total_segment_count;
    value["restored_segment_count"] = snapshot.restored_segment_count;
    value["remaining_segment_count"] = snapshot.remaining_segment_count;
    value["failed_segment_count"] = snapshot.failed_segment_count;
    value["segment_progress_percent"] = snapshot.segment_progress_percent;
    value["expires_at"] = static_cast<Json::Int64>(snapshot.expires_at);
    return value;
}

Json::Value TierStorageBase::tieringJobToJson(const TieringJob &job,
                                              const std::string &camera_name,
                                              bool detail) {
    Json::Value value = job.toJson();
    value["progress_percent"] = progressPercent(job.bytes_moved, job.bytes_total, job.status);
    if (!detail)
        value["camera_name"] = camera_name;
    value["segment_count"] = 0;
    value["processed_segment_count"] = 0;
    value["total_bytes"] = static_cast<Json::Int64>(job.bytes_total);
    value["processed_bytes"] = static_cast<Json::Int64>(job.bytes_moved);
    value.removeMember("segment_start_time");
    value.removeMember("segment_end_time");
    value.removeMember("bytes_total");
    value.removeMember("bytes_moved");
    if (detail) {
        const std::string error_message = job.error_message.value_or("");
        const bool policy_changed = job.status == "CANCELLED" &&
            error_message.find("POLICY_CHANGED:") == 0;
        value["error_code"] = error_message.empty()
            ? "" : (policy_changed ? "TIERING_JOB_POLICY_CHANGED" : "TIERING_JOB_FAILED");
        value["error_message"] = error_message;
        value["retryable"] = (job.status == "FAILED");
    } else {
        value.removeMember("error_message");
    }
    return value;
}

Json::Value TierStorageBase::alertToJson(const StorageAlert &alert,
                                         const StoragePoolMap &pool_map) {
    Json::Value value = alert.toJson();
    auto pool_it = pool_map.find(alert.pool_id);
    value["pool_name"] = pool_it == pool_map.end() ? "" : pool_it->second.name;
    return value;
}

Json::Value TierStorageBase::expiredSegmentToJson(const SegmentTierRange &range,
                                                  const std::string &camera_name,
                                                  bool is_protected) {
    Json::Value value = range.toJson();
    value["segment_id"] = range.range_id;
    value["camera_name"] = camera_name;
    value["expired_at"] = static_cast<Json::Int64>(
        range.updated_at > 0 ? range.updated_at : range.end_time);
    value["protected"] = is_protected;
    value["evidence"] = false;
    value.removeMember("status");
    return value;
}

Json::Value TierStorageBase::protectedVideoToJson(const ProtectedVideo &video,
                                                  const std::string &camera_name) {
    Json::Value value = video.toJson();
    value["camera_name"] = camera_name;
    return value;
}

std::string TierStorageBase::worstStorageStatus(const std::string &left,
                                                const std::string &right) {
    const auto rank = [](const std::string &status) {
        if (status == "CRITICAL" || status == "OFFLINE") return 4;
        if (status == "HIGH") return 3;
        if (status == "WARNING") return 2;
        return 1;
    };
    return rank(right) > rank(left) ? right : left;
}

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
