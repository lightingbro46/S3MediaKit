#ifndef STORAGE_POLICYASSIGNMENT_H
#define STORAGE_POLICYASSIGNMENT_H

#include <string>
#include <vector>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/util.h"

namespace managerkit {

// ===================================================================
// CameraPolicyAssignment — camera-level policy override
// ===================================================================
struct CameraPolicyAssignment {
    std::string camera_id;
    std::string policy_id;
    Optional<std::string> override_reason;
    int64_t assigned_at = 0;
};

DECLARE_ENTITY(CameraPolicyAssignment, "camera_policy_assignments",
    {"camera_id"},
    &CameraPolicyAssignment::camera_id,       "camera_id",
    &CameraPolicyAssignment::policy_id,       "policy_id",
    &CameraPolicyAssignment::override_reason, "override_reason",
    &CameraPolicyAssignment::assigned_at,     "assigned_at"
)

// ===================================================================
// Repository
// ===================================================================
class PolicyAssignmentRepository : public SqliteRepository<CameraPolicyAssignment> {
public:
    PolicyAssignmentRepository() : SqliteRepository<CameraPolicyAssignment>(Database::kMediaServerDb) {}

    std::vector<CameraPolicyAssignment> findByCameraId(const std::string &camera_id) {
        CameraPolicyAssignment a;
        a.camera_id = camera_id;
        return SqliteRepository<CameraPolicyAssignment>::findById(a);
    }

    bool deleteByCameraId(const std::string &camera_id) {
        CameraPolicyAssignment a;
        a.camera_id = camera_id;
        return SqliteRepository<CameraPolicyAssignment>::removeById(a);
    }

    std::vector<std::string> findCamerasByPolicyId(const std::string &policy_id) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"camera_id"})
                .from(EntityTraits<CameraPolicyAssignment>::tableName())
                .where("policy_id = ?", {policy_id}));
        std::vector<std::string> ids;
        for (const auto &row : rows)
            if (!row.empty()) ids.push_back(row[0]);
        return ids;
    }
};

// ===================================================================
// IMP
// ===================================================================
class PolicyAssignmentImp : public PolicyAssignmentRepository {
public:
    using Ptr = std::shared_ptr<PolicyAssignmentImp>;

    bool assign(const CameraPolicyAssignment &a) {
        // upsert: remove existing then insert
        deleteByCameraId(a.camera_id);
        return save(a, true);
    }

    bool remove(const std::string &camera_id) {
        return deleteByCameraId(camera_id);
    }
};

} // namespace managerkit

#endif // STORAGE_POLICYASSIGNMENT_H
