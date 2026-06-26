#ifndef STORAGE_STORAGEPOOL_H
#define STORAGE_STORAGEPOOL_H

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

    // Count how many policies reference this pool (for delete guard)
    int countPolicyReferences(const std::string &pool_id) {
        auto sql = "SELECT COUNT(*) FROM storage_policies WHERE tiers_json LIKE ?";
        auto rows = _executor->executeRaw(std::string(sql));
        // Simple scan approach: load all policies and check JSON
        // (policy count is small, this is acceptable)
        auto qRows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"tiers_json"})
                .from("storage_policies")
                .where("enabled = ?", {"1"}));
        int count = 0;
        for (const auto &row : qRows) {
            if (!row.empty() && row[0].find(pool_id) != std::string::npos)
                ++count;
        }
        return count;
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
};

} // namespace managerkit

#endif // STORAGE_STORAGEPOOL_H
