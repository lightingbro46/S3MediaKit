#ifndef LOCAL_TIER_STORAGE_BASE_H
#define LOCAL_TIER_STORAGE_BASE_H

#ifdef ENABLE_TIER_STORAGE

#include <string>
#include <unordered_map>
#include <vector>

#include "Storage/StoragePool.h"
#include "Storage/StoragePolicy.h"
#include "Storage/StorageTierExtra.h"
#include "Storage/TieringJob.h"

namespace managerkit {

class TierStorageBase {
public:
    using StoragePoolMap = std::unordered_map<std::string, StoragePool>;

    static StoragePool poolFromJson(const Json::Value &body);
    static void applyPoolPatch(StoragePool &pool, const Json::Value &body);
    static bool validatePoolByType(const StoragePool &pool, std::string &error);

    static StoragePolicy policyFromJson(const Json::Value &body);
    static bool validatePolicyTierMoveChain(const std::vector<PolicyTierConfig> &tiers,
                                            std::string &error);
    static bool validatePolicyTiers(const std::vector<PolicyTierConfig> &tiers,
                                    std::string &error);
    static bool validateStoragePolicy(const StoragePolicy &policy,
                                      const StoragePoolMap &pool_map,
                                      std::string &error);

    static StoragePoolMap loadPoolMap();
    static bool isRestoreRequiredPool(const std::string &pool_id,
                                      const StoragePoolMap &pool_map);

    static Json::Value poolSummaryToJson(const StoragePool &pool);
    static Json::Value poolDetailToJson(const StoragePool &pool);
    static Json::Value policySummaryToJson(const StoragePolicy &policy,
                                           int applied_camera_count);
    static Json::Value policyDetailToJson(const StoragePolicy &policy,
                                          const StoragePoolMap &pool_map,
                                          bool include_summary_fields = false);
    static Json::Value effectivePolicyToJson(const EffectivePolicyResult &result,
                                             const std::string &camera_name);
    static Json::Value restoreJobToJson(const RestoreJob &job,
                                        const std::string &camera_name,
                                        bool list_view = false);
    static Json::Value restoreJobSnapshotToJson(const RestoreJobSnapshot &snapshot,
                                                const std::string &camera_name = "");
    static Json::Value tieringJobToJson(const TieringJob &job,
                                        const std::string &camera_name,
                                        bool detail = false);
    static Json::Value alertToJson(const StorageAlert &alert,
                                   const StoragePoolMap &pool_map);
    static Json::Value expiredSegmentToJson(const SegmentTierRange &range,
                                            const std::string &camera_name,
                                            bool is_protected);
    static Json::Value protectedVideoToJson(const ProtectedVideo &video,
                                            const std::string &camera_name);
    static std::string worstStorageStatus(const std::string &left,
                                          const std::string &right);

private:
    TierStorageBase();
};

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE

#endif // LOCAL_TIER_STORAGE_BASE_H
