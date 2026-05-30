#ifndef STORAGE_RESOURCE_ASSIGNMENT_H
#define STORAGE_RESOURCE_ASSIGNMENT_H

#include <string>
#include "DbStorage.h"
#include "TransactionLog.h"

namespace managerkit {

enum class ResourceAssignType : uint8_t {
    PRIMARY = 0,
    FAILOVER = 1,
    MANUAL = 2
};

struct VmsResourceAssignment {
    std::string assignment_guid;
    std::string resource_guid;
    std::string owner_peer_id;
    std::string owner_db_guid;
    int64_t assigned_at;
    int64_t released_at;
    int assign_type;
    std::string prev_peer_id;

    Json::Value toJson() const {
        Json::Value v;
        v["assignment_guid"] = assignment_guid;
        v["resource_guid"]   = resource_guid;
        v["owner_peer_id"]   = owner_peer_id;
        v["owner_db_guid"]   = owner_db_guid;
        v["assigned_at"]     = assigned_at;
        v["released_at"]     = released_at;
        v["assign_type"]     = assign_type;
        v["prev_peer_id"]    = prev_peer_id;
        return v;
    }
};

DECLARE_ENTITY(VmsResourceAssignment, "vms_resource_assignment",
    {"assignment_guid"},
    &VmsResourceAssignment::assignment_guid, "assignment_guid",
    &VmsResourceAssignment::resource_guid, "resource_guid",
    &VmsResourceAssignment::owner_peer_id, "owner_peer_id",
    &VmsResourceAssignment::owner_db_guid, "owner_db_guid",
    &VmsResourceAssignment::assigned_at, "assigned_at",
    &VmsResourceAssignment::released_at, "released_at",
    &VmsResourceAssignment::assign_type, "assign_type",
    &VmsResourceAssignment::prev_peer_id, "prev_peer_id"
)

class VmsResourceAssignmentRepository : public SqliteRepository<VmsResourceAssignment> {
public:
    VmsResourceAssignmentRepository() : SqliteRepository<VmsResourceAssignment>(Database::kEdgeStorageControllerDb) {}
};

class VmsResourceAssignmentImp : public VmsResourceAssignmentRepository {
public:
    using Ptr = std::shared_ptr<VmsResourceAssignmentImp>;
    VmsResourceAssignmentImp() : VmsResourceAssignmentRepository() {
        _log_impl = std::make_shared<TransactionLogImp>();
    }

    void add(VmsResourceAssignment &assign, bool append_log = true) {
        if (assign.assignment_guid.empty()) {
            assign.assignment_guid = toolkit::makeUuidStr();
        }
        save(assign, true);
        if (append_log) {
            _log_impl->appendLocalDataMutation(EntityTraits<VmsResourceAssignment>::tableName(), TRAN_DATA_OP_UPSERT, assign.toJson());
        }
    }

    void update(const VmsResourceAssignment &assign, bool append_log = true) {
        updateById(assign);
        if (append_log) {
            _log_impl->appendLocalDataMutation(EntityTraits<VmsResourceAssignment>::tableName(), TRAN_DATA_OP_UPSERT, assign.toJson());
        }
    }

    void remove(const std::string &assignment_guid, bool append_log = true) {
        VmsResourceAssignment assign;
        assign.assignment_guid = assignment_guid;
        removeById(assign);
        if (append_log) {
            Json::Value payload;
            payload["assignment_guid"] = assignment_guid;
            _log_impl->appendLocalDataMutation(EntityTraits<VmsResourceAssignment>::tableName(), TRAN_DATA_OP_DELETE, payload);
        }
    }

    std::vector<VmsResourceAssignment> findAssignmentByRange(const std::string &resource_guid, int64_t start_time, int64_t end_time) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_guid = ? AND assigned_at BETWEEN ? AND ?";
        whereParams.push_back(resource_guid);
        whereParams.push_back(std::to_string(start_time));
        whereParams.push_back(std::to_string(end_time));

        auto query = toolkit::QueryBuilder()
                            .select(EntityTraits<VmsResourceAssignment>::getColumns())
                            .from(EntityTraits<VmsResourceAssignment>::tableName())
                            .where(whereClause.str(), whereParams);
        std::vector<VmsResourceAssignment> ret;
        auto rows = _executor->executeRaw(query);
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<VmsResourceAssignment>::fromRow(row));
        }
        return ret;
    }

    std::vector<VmsResourceAssignment> findCurrentAssign(const std::string &resource_guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_guid = ?";
        whereParams.push_back(resource_guid);

        auto query = toolkit::QueryBuilder()
                            .select(EntityTraits<VmsResourceAssignment>::getColumns())
                            .from(EntityTraits<VmsResourceAssignment>::tableName())
                            .where(whereClause.str(), whereParams)
                            .orderBy("assigned_at DESC")
                            .limit(1);
        std::vector<VmsResourceAssignment> ret;
        auto rows = _executor->executeRaw(query);
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<VmsResourceAssignment>::fromRow(row));
        }
        return ret;
    }

private:
    TransactionLogImp::Ptr _log_impl;
};

} // namespace managerkit

#endif // STORAGE_RESOURCE_ASSIGNMENT_H