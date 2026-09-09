#ifndef STORAGE_STORAGEPOOL_H
#define STORAGE_STORAGEPOOL_H

#ifdef ENABLE_TIER_STORAGE

#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/util.h"
#include "Local/StorageTier.h"

namespace managerkit {

// ===================================================================
// StoragePool entity — one row per storage backend
// ===================================================================
struct StoragePool {
    std::string id;
    std::string name;
    std::string type;                        // LOCAL_DISK | NAS | MINIO | S3 | ARCHIVE
    std::string tier;                        // HOT | WARM | COLD
    Optional<std::string> endpoint;
    Optional<std::string> bucket;
    Optional<std::string> base_path;
    Optional<std::string> access_key;
    Optional<std::string> secret_key_enc;    // stored encrypted (or as-is for now)
    // Logical filesystem roots. They exclude Record::kAppName; tier storage
    // resolves the effective data directory before performing filesystem I/O.
    Optional<std::string> mount_path;
    Optional<std::string> network_path;
    int     enabled                    = 1;
    int     health_check_enabled       = 1;
    int     high_watermark_percent     = 85;
    int     critical_watermark_percent = 90;
    int64_t created_at                 = 0;
    int64_t updated_at                 = 0;

    // Runtime-only fields (not stored in DB, populated by fillPoolRuntimeStats)
    std::string health_status;
    int64_t     used_bytes       = 0;
    int64_t     total_bytes      = 0;
    float       usage_pct        = 0.0f;
    int64_t     last_health_check = 0;
    bool        capacity_available = false;
    std::string capacity_status = "UNAVAILABLE";
    std::string capacity_message;

    Json::Value toJson() const {
        Json::Value v;
        v["id"]                        = id;
        v["name"]                      = name;
        v["type"]                      = type;
        v["tier"]                      = tier;
        v["endpoint"]                  = endpoint.value_or("");
        v["bucket"]                    = bucket.value_or("");
        v["base_path"]                 = base_path.value_or("");
        v["access_key"]                = access_key.value_or("");
        // secret_key is never returned to callers
        v["mount_path"]                = mount_path.value_or("");
        v["network_path"]              = network_path.value_or("");
        v["enabled"]                   = (enabled != 0);
        v["health_check_enabled"]      = (health_check_enabled != 0);
        v["high_watermark_percent"]    = high_watermark_percent;
        v["critical_watermark_percent"] = critical_watermark_percent;
        v["created_at"]                = static_cast<Json::Int64>(created_at);
        v["updated_at"]                = static_cast<Json::Int64>(updated_at);
        v["health_status"]             = health_status.empty() ? "OK" : health_status;
        v["used_bytes"]                = static_cast<Json::Int64>(used_bytes);
        v["total_bytes"]               = static_cast<Json::Int64>(total_bytes);
        v["usage_pct"]                 = usage_pct;
        v["last_health_check"]         = static_cast<Json::Int64>(last_health_check);
        v["capacity_available"]        = capacity_available;
        v["capacity_status"]           = capacity_status;
        v["capacity_message"]          = capacity_message;
        return v;
    }
};

DECLARE_ENTITY(StoragePool, "storage_pools",
    {"id"},
    &StoragePool::id,                     "id",
    &StoragePool::name,                   "name",
    &StoragePool::type,                   "type",
    &StoragePool::tier,                   "tier",
    &StoragePool::endpoint,               "endpoint",
    &StoragePool::bucket,                 "bucket",
    &StoragePool::base_path,              "base_path",
    &StoragePool::access_key,             "access_key",
    &StoragePool::secret_key_enc,         "secret_key_enc",
    &StoragePool::mount_path,             "mount_path",
    &StoragePool::network_path,           "network_path",
    &StoragePool::enabled,                "enabled",
    &StoragePool::health_check_enabled,   "health_check_enabled",
    &StoragePool::high_watermark_percent, "high_watermark_percent",
    &StoragePool::critical_watermark_percent, "critical_watermark_percent",
    &StoragePool::created_at,             "created_at",
    &StoragePool::updated_at,             "updated_at"
)

// ===================================================================
// Repository
// ===================================================================
class StoragePoolRepository : public SqliteRepository<StoragePool> {
public:
    StoragePoolRepository() : SqliteRepository<StoragePool>(Database::kMediaServerDb) {}

    std::vector<StoragePool> queryAll(const std::string &tier     = "",
                                     const std::string &type     = "",
                                     const std::string &keyword  = "") {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;

        auto add = [&](const std::string &cond, const std::string &val) {
            if (!val.empty()) {
                if (has) where << " AND ";
                where << cond;
                params.push_back(val);
                has = true;
            }
        };

        add("tier = ?",       tier);
        add("type = ?",       type);
        if (!keyword.empty()) {
            if (has) where << " AND ";
            where << "name LIKE ?";
            params.push_back("%" + keyword + "%");
            has = true;
        }

        auto q = toolkit::QueryBuilder()
            .select(EntityTraits<StoragePool>::getColumns())
            .from(EntityTraits<StoragePool>::tableName());
        if (has) q = q.where(where.str(), params);

        auto rows = _executor->executeRaw(q);
        std::vector<StoragePool> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<StoragePool>::fromRow(row));
        return ret;
    }

    std::vector<StoragePool> findByPoolId(const std::string &pool_id) {
        StoragePool p;
        p.id = pool_id;
        return SqliteRepository<StoragePool>::findById(p);
    }

    bool deleteByPoolId(const std::string &pool_id) {
        StoragePool p;
        p.id = pool_id;
        return SqliteRepository<StoragePool>::removeById(p);
    }

    std::vector<std::string> findPolicyTierConfigs() {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"tiers_json"})
                .from("storage_policies"));
        std::vector<std::string> configs;
        configs.reserve(rows.size());
        for (const auto &row : rows) {
            if (!row.empty())
                configs.push_back(row[0]);
        }
        return configs;
    }
};

// ===================================================================
// IMP — public facade used by TierStorageManager
// ===================================================================
class StoragePoolImp : public StoragePoolRepository {
public:
    using Ptr = std::shared_ptr<StoragePoolImp>;

    bool add(const StoragePool &pool)     { return save(pool, /*include_id=*/true); }
    bool update(const StoragePool &pool)  { return updateById(pool); }
    bool remove(const std::string &id)    { return deleteByPoolId(id); }

    // Policy JSON is domain data. Repository only loads the stored payload;
    // the facade parses it and performs exact pool-id matching.
    int countPolicyReferences(const std::string &pool_id) {
        int count = 0;
        const auto configs = findPolicyTierConfigs();
        for (const auto &config : configs) {
            Json::Value tiers;
            Json::Reader reader;
            if (!reader.parse(config, tiers) || !tiers.isArray())
                continue;
            for (Json::ArrayIndex i = 0; i < tiers.size(); ++i) {
                if (tiers[i].isObject() && tiers[i]["pool_id"].asString() == pool_id) {
                    ++count;
                    break;
                }
            }
        }
        return count;
    }
};

} // namespace managerkit

#endif // STORAGE_STORAGEPOOL_H

#endif // ENABLE_TIER_STORAGE
