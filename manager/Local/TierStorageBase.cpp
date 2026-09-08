#include "TierStorageBase.h"

#ifdef ENABLE_TIER_STORAGE

#include "StorageTier.h"

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

    applyOptionalString(pool.endpoint, body, "endpoint");
    applyOptionalString(pool.bucket, body, "bucket");
    applyOptionalString(pool.base_path, body, "base_path");
    applyOptionalString(pool.access_key, body, "access_key", true);
    applyOptionalString(pool.secret_key_enc, body, "secret_key", true);
    applyOptionalString(pool.mount_path, body, "mount_path");
    applyOptionalString(pool.network_path, body, "network_path");
}

bool TierStorageBase::validatePoolByType(const StoragePool &pool, std::string &error) {
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
    return true;
}

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
