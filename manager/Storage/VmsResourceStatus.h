#ifndef S3MANAGERKIT_VMSRESOURCESTATUS_H
#define S3MANAGERKIT_VMSRESOURCESTATUS_H

#include <string>
#include <mutex>
#include "DbStorage.h"
#include "Util/util.h"
#include "json/json.h"
#include "TransactionLog.h"
#include "Extension/TableSyncHandler.h"

namespace managerkit {

struct VmsResourceStatus {
    std::string guid;
    int status;

    Json::Value toJson() const {
        Json::Value v;
        v["guid"] = guid;
        v["status"] = status;
        return v;
    }

    static VmsResourceStatus fromJson(const Json::Value &v) {
        VmsResourceStatus resource;
        resource.guid = v["guid"].asString();
        resource.status = v["status"].asInt();
        return resource;
    }
};

DECLARE_ENTITY(VmsResourceStatus, "vms_resource_status",
    {"guid"},
    &VmsResourceStatus::guid, "guid", 
    &VmsResourceStatus::status, "status"
)

class VmsResourceStatusRepository : public SqliteRepository<VmsResourceStatus> {
public:
    VmsResourceStatusRepository() : SqliteRepository<VmsResourceStatus>(Database::kEdgeStorageControllerDb) {}

protected:
    std::vector<VmsResourceStatus> findByGuid(const std::string &guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "guid = ?";
        whereParams.push_back(guid);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<VmsResourceStatus>::getColumns())
                         .from(EntityTraits<VmsResourceStatus>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<VmsResourceStatus> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VmsResourceStatus>::fromRow(row));
        }
        return ret;
    }

    std::vector<VmsResourceStatus> findByGuidWithTxn(const std::string &guid, toolkit::SqliteTransaction::Ptr txn) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<VmsResourceStatus>::getColumns())
                         .from(EntityTraits<VmsResourceStatus>::tableName())
                         .where("guid = ?", {guid});
        auto rows = executeRawWithTxn(txn, query);
        std::vector<VmsResourceStatus> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VmsResourceStatus>::fromRow(row));
        }
        return ret;
    }

    bool removeByGuid(const std::string &guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "guid = ?";
        whereParams.push_back(guid);

        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<VmsResourceStatus>::tableName())
                            .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }

    bool removeByGuidWithTxn(const std::string &guid, toolkit::SqliteTransaction::Ptr txn) {
        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<VmsResourceStatus>::tableName())
                            .where("guid = ?", {guid});
        return execDMLWithTxn(txn, query);
    }
};

class VmsResourceStatusImp : public VmsResourceStatusRepository {
public:
    using Ptr = std::shared_ptr<VmsResourceStatusImp>;
    VmsResourceStatusImp() : VmsResourceStatusRepository() {
        _log_impl = std::make_shared<TransactionLogImp>();
    }

    void add(VmsResourceStatus &resource, bool append_log = true) {
        static std::mutex s_add_mtx;
        std::lock_guard<std::mutex> lk(s_add_mtx);
        auto ret = findByGuid(resource.guid);
        if (ret.size() > 0) {
            if (ret[0].status == resource.status) return;
            updateById(resource);
        } else {
            save(resource, true);
        }
        if (append_log) {
            _log_impl->appendLocalDataMutation(EntityTraits<VmsResourceStatus>::tableName(), TRAN_DATA_OP_UPSERT, resource.toJson());
        }
    }

    void remove(const std::string &guid, bool append_log = true) {
        removeByGuid(guid);
        if (append_log) {
            Json::Value payload;
            payload["guid"] = guid;
            _log_impl->appendLocalDataMutation(EntityTraits<VmsResourceStatus>::tableName(), TRAN_DATA_OP_DELETE, payload);
        }
    }

    // Transaction-aware variants.
    void addWithTxn(VmsResourceStatus &resource, toolkit::SqliteTransaction::Ptr txn) {
        static std::mutex s_add_mtx;
        std::lock_guard<std::mutex> lk(s_add_mtx);
        auto ret = findByGuidWithTxn(resource.guid, txn);
        if (!ret.empty()) {
            if (ret[0].status == resource.status) return;
            updateByIdWithTxn(resource, txn);
        } else {
            saveWithTxn(resource, txn, true);
        }
    }

    void removeWithTxn(const std::string &guid, toolkit::SqliteTransaction::Ptr txn) {
        removeByGuidWithTxn(guid, txn);
    }

    static TableSyncHandler makeSyncHandler() {
        TableSyncHandler h;
        h.rowKey = [](const Json::Value &p) -> std::string { return p["guid"].asString(); };
        h.onUpsert = [](const Json::Value &p) {
            auto imp = std::make_shared<VmsResourceStatusImp>();
            auto status = VmsResourceStatus::fromJson(p);
            imp->add(status, false);
        };
        h.onDelete = [](const Json::Value &p) {
            auto imp = std::make_shared<VmsResourceStatusImp>();
            auto guid = p["guid"].asString();
            if (!guid.empty()) imp->remove(guid, false);
        };
        h.onSnapshot = [](const Json::Value &arr) {
            auto imp = std::make_shared<VmsResourceStatusImp>();
            for (const auto &v : arr) {
                auto status = VmsResourceStatus::fromJson(v);
                imp->add(status, false);
            }
        };
        h.onUpsertWithTxn = [](const Json::Value &p, toolkit::SqliteTransaction::Ptr txn) {
            auto imp = std::make_shared<VmsResourceStatusImp>();
            auto status = VmsResourceStatus::fromJson(p);
            imp->addWithTxn(status, txn);
        };
        h.onDeleteWithTxn = [](const Json::Value &p, toolkit::SqliteTransaction::Ptr txn) {
            auto imp = std::make_shared<VmsResourceStatusImp>();
            auto guid = p["guid"].asString();
            if (!guid.empty()) imp->removeWithTxn(guid, txn);
        };
        return h;
    }

    int findStatus(const std::string &guid) {
        auto ret = findByGuid(guid);
        if (ret.size() > 0) {
            return ret[0].status;
        }
        return 0;
    }

private:
    TransactionLogImp::Ptr _log_impl;
};

} // namespace managerkit

#endif // S3MANAGERKIT_VMSRESOURCESTATUS_H
