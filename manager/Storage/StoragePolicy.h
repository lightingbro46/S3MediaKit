#ifndef STORAGE_STORAGEPOLICY_H
#define STORAGE_STORAGEPOLICY_H

#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/util.h"
#include "Local/StorageTier.h"

namespace managerkit {

// ===================================================================
// StoragePolicy entity
// Tier configs, delete rules and advanced rules are serialised as JSON.
// ===================================================================
struct StoragePolicy {
    std::string id;
    std::string name;
    Optional<std::string> description;
    int     enabled               = 1;
    int     total_retention_days  = 30;
    int     allow_camera_override = 1;
    int     protect_event_video   = 0;
    std::string tiers_json;           // JSON array of PolicyTierConfig
    std::string delete_policy_json;   // JSON object PolicyDeleteConfig
    std::string advanced_rules_json;  // JSON object PolicyAdvancedRules
    int64_t created_at            = 0;
    int64_t updated_at            = 0;

    // ---- parsed helpers (not stored) ----
    std::vector<PolicyTierConfig> parsedTiers() const {
        std::vector<PolicyTierConfig> result;
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(tiers_json.empty() ? "[]" : tiers_json, root) && root.isArray()) {
            for (const auto &t : root)
                result.push_back(PolicyTierConfig::fromJson(t));
        }
        return result;
    }

    PolicyDeleteConfig parsedDeletePolicy() const {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(delete_policy_json.empty() ? "{}" : delete_policy_json, root))
            return PolicyDeleteConfig::fromJson(root);
        return {};
    }

    PolicyAdvancedRules parsedAdvancedRules() const {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(advanced_rules_json.empty() ? "{}" : advanced_rules_json, root))
            return PolicyAdvancedRules::fromJson(root);
        return {};
    }

    static std::string tiersToJson(const std::vector<PolicyTierConfig> &tiers) {
        Json::Value arr(Json::arrayValue);
        for (const auto &t : tiers)
            arr.append(t.toJson());
        Json::FastWriter writer;
        return writer.write(arr);
    }

    Json::Value toJson() const {
        Json::Value v;
        v["id"]                   = id;
        v["name"]                 = name;
        v["description"]          = description.value_or("");
        v["enabled"]              = (enabled != 0);
        v["total_retention_days"] = total_retention_days;
        v["allow_camera_override"]= (allow_camera_override != 0);
        v["protect_event_video"]  = (protect_event_video != 0);
        v["created_at"]           = static_cast<Json::Int64>(created_at);
        v["updated_at"]           = static_cast<Json::Int64>(updated_at);

        // parse & embed nested objects
        Json::Value tiersArr(Json::arrayValue);
        for (const auto &t : parsedTiers())
            tiersArr.append(t.toJson());
        v["tiers"] = tiersArr;
        v["delete_policy"]    = parsedDeletePolicy().toJson();
        v["advanced_rules"]   = parsedAdvancedRules().toJson();
        return v;
    }
};

DECLARE_ENTITY(StoragePolicy, "storage_policies",
    {"id"},
    &StoragePolicy::id,                  "id",
    &StoragePolicy::name,                "name",
    &StoragePolicy::description,         "description",
    &StoragePolicy::enabled,             "enabled",
    &StoragePolicy::total_retention_days,"total_retention_days",
    &StoragePolicy::allow_camera_override,"allow_camera_override",
    &StoragePolicy::protect_event_video, "protect_event_video",
    &StoragePolicy::tiers_json,          "tiers_json",
    &StoragePolicy::delete_policy_json,  "delete_policy_json",
    &StoragePolicy::advanced_rules_json, "advanced_rules_json",
    &StoragePolicy::created_at,          "created_at",
    &StoragePolicy::updated_at,          "updated_at"
)

// ===================================================================
// Repository
// ===================================================================
class StoragePolicyRepository : public SqliteRepository<StoragePolicy> {
public:
    StoragePolicyRepository() : SqliteRepository<StoragePolicy>(Database::kMediaServerDb) {}

    std::vector<StoragePolicy> queryAll(const std::string &keyword = "",
                                        int enabled_filter = -1,   // -1 = no filter
                                        int page = 0, int size = 20) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;

        if (enabled_filter >= 0) {
            where << "enabled = ?";
            params.push_back(std::to_string(enabled_filter));
            has = true;
        }
        if (!keyword.empty()) {
            if (has) where << " AND ";
            where << "name LIKE ?";
            params.push_back("%" + keyword + "%");
            has = true;
        }

        auto q = toolkit::QueryBuilder()
            .select(EntityTraits<StoragePolicy>::getColumns())
            .from(EntityTraits<StoragePolicy>::tableName())
            .orderBy("created_at DESC")
            .limit(size)
            .offset(page * size);
        if (has) q = q.where(where.str(), params);

        auto rows = _executor->executeRaw(q);
        std::vector<StoragePolicy> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<StoragePolicy>::fromRow(row));
        return ret;
    }

    int countAll(const std::string &keyword = "", int enabled_filter = -1) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;

        if (enabled_filter >= 0) {
            where << "enabled = ?";
            params.push_back(std::to_string(enabled_filter));
            has = true;
        }
        if (!keyword.empty()) {
            if (has) where << " AND ";
            where << "name LIKE ?";
            params.push_back("%" + keyword + "%");
            has = true;
        }

        auto q = toolkit::QueryBuilder()
            .select({"COUNT(*)"})
            .from(EntityTraits<StoragePolicy>::tableName());
        if (has) q = q.where(where.str(), params);

        auto rows = _executor->executeRaw(q);
        if (!rows.empty() && !rows[0].empty())
            return std::stoi(rows[0][0]);
        return 0;
    }

    std::vector<StoragePolicy> findByPolicyId(const std::string &policy_id) {
        StoragePolicy p;
        p.id = policy_id;
        return SqliteRepository<StoragePolicy>::findById(p);
    }

    bool deleteByPolicyId(const std::string &policy_id) {
        StoragePolicy p;
        p.id = policy_id;
        return SqliteRepository<StoragePolicy>::removeById(p);
    }

    // How many cameras are assigned this policy
    int countCameraAssignments(const std::string &policy_id) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"COUNT(*)"})
                .from("camera_policy_assignments")
                .where("policy_id = ?", {policy_id}));
        if (!rows.empty() && !rows[0].empty())
            return std::stoi(rows[0][0]);
        return 0;
    }
};

// ===================================================================
// IMP
// ===================================================================
class StoragePolicyImp : public StoragePolicyRepository {
public:
    using Ptr = std::shared_ptr<StoragePolicyImp>;

    bool add(const StoragePolicy &policy)    { return save(policy, true); }
    bool update(const StoragePolicy &policy) { return updateById(policy); }
    bool remove(const std::string &id)       { return deleteByPolicyId(id); }
};

} // namespace managerkit

#endif // STORAGE_STORAGEPOLICY_H
