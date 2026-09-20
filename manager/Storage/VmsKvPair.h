#ifndef S3MANAGERKIT_VMSKVPAIR_H
#define S3MANAGERKIT_VMSKVPAIR_H

#include <string>
#include <mutex>
#include <set>
#include "DbStorage.h"
#include "TransactionLog.h"
#include "Util/util.h"
#include "Extension/TableSyncHandler.h"

namespace managerkit {

struct VmsKvPair {
    std::string id;
    std::string resource_guid;
    std::string name;
    std::string value;

    Json::Value toJson() const {
        Json::Value v;
        v["id"]            = id;
        v["resource_guid"] = resource_guid;
        v["name"]          = name;
        v["value"]         = value;
        return v;
    }

    static VmsKvPair fromJson(const Json::Value &v) {
        VmsKvPair kv;
        kv.id            = v["id"].asString();
        kv.resource_guid = v["resource_guid"].asString();
        kv.name          = v["name"].asString();
        kv.value         = v["value"].asString();
        return kv;
    }
};

DECLARE_ENTITY(VmsKvPair, "vms_kvpair",
    {"id"},
    &VmsKvPair::id, "id", 
    &VmsKvPair::resource_guid, "resource_guid", 
    &VmsKvPair::name, "name", 
    &VmsKvPair::value, "value"
)

class VmsKvPairRepository : public SqliteRepository<VmsKvPair> {
public:
    VmsKvPairRepository() : SqliteRepository<VmsKvPair>(Database::kEdgeStorageControllerDb) {}

protected:
    std::vector<VmsKvPair> findByResourceId(const std::string &guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_guid = ?";
        whereParams.push_back(guid);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<VmsKvPair>::getColumns())
                         .from(EntityTraits<VmsKvPair>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<VmsKvPair> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VmsKvPair>::fromRow(row));
        }
        return ret;
    }

    std::vector<VmsKvPair> findByResourceIdAndKey(const std::string &guid, const std::string &key) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_guid = ? AND name = ?";
        whereParams.push_back(guid);
        whereParams.push_back(key);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<VmsKvPair>::getColumns())
                         .from(EntityTraits<VmsKvPair>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<VmsKvPair> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VmsKvPair>::fromRow(row));
        }
        return ret;
    }

    std::vector<VmsKvPair> findByResourceIdAndKeyWithTxn(const std::string &guid, const std::string &key, toolkit::SqliteTransaction::Ptr txn) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<VmsKvPair>::getColumns())
                         .from(EntityTraits<VmsKvPair>::tableName())
                         .where("resource_guid = ? AND name = ?", {guid, key});
        auto rows = executeRawWithTxn(txn, query);
        std::vector<VmsKvPair> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VmsKvPair>::fromRow(row));
        }
        return ret;
    }

    bool removeByResourceId(const std::string &guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_guid = ?";
        whereParams.push_back(guid);

        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<VmsKvPair>::tableName())
                            .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }

    // Deletes the kv rows of a resource except the listed names.
    bool removeByResourceIdExceptWithTxn(const std::string &guid, const std::set<std::string> &keep_names,
                                         toolkit::SqliteTransaction::Ptr txn) {
        if (keep_names.empty()) {
            return removeByResourceIdWithTxn(guid, txn);
        }
        std::string where = "resource_guid = ? AND name NOT IN (";
        std::vector<std::string> params{guid};
        for (const auto &name : keep_names) {
            where += params.size() > 1 ? ",?" : "?";
            params.push_back(name);
        }
        where += ")";
        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<VmsKvPair>::tableName())
                            .where(where, params);
        return execDMLWithTxn(txn, query);
    }

    bool removeByResourceIdWithTxn(const std::string &guid, toolkit::SqliteTransaction::Ptr txn) {
        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<VmsKvPair>::tableName())
                            .where("resource_guid = ?", {guid});
        return execDMLWithTxn(txn, query);
    }
};

class VmsKvPairImp : public VmsKvPairRepository {
public:
    using Ptr = std::shared_ptr<VmsKvPairImp>;

    VmsKvPairImp() : VmsKvPairRepository() {
        _log_impl = std::make_shared<TransactionLogImp>();
    }

    // Single upsert — giữ backward compat, ghi 1 log nếu changed
    void add(VmsKvPair &kv, bool append_log = true) {
        auto ret = findByResourceIdAndKey(kv.resource_guid, kv.name);
        bool changed = false;
        if (!ret.empty()) {
            kv.id = ret[0].id;
            if (ret[0].value != kv.value) {
                updateById(kv);
                changed = true;
            }
        } else {
            save(kv);
            changed = true;
        }
        if (changed && append_log) {
            _log_impl->appendLocalDataMutation(EntityTraits<VmsKvPair>::tableName(), TRAN_DATA_OP_UPSERT, kv.toJson());
        }
    }

    // Batch upsert — chỉ ghi 1 transaction log cho toàn bộ batch
    // Payload: {"resource_guid":"...","items":[{kv1},{kv2},...]}
    void addBatch(const std::vector<VmsKvPair> &kvs, bool append_log = true) {
        if (kvs.empty()) return;

        Json::Value changed_items = Json::arrayValue;
        for (auto kv : kvs) {
            if (addInternal(kv)) {
                changed_items.append(kv.toJson());
            }
        }

        if (append_log && changed_items.size() > 0) {
            Json::Value payload;
            payload["resource_guid"] = kvs[0].resource_guid;
            payload["items"]         = changed_items;
            _log_impl->appendLocalDataMutation(EntityTraits<VmsKvPair>::tableName(), TRAN_DATA_OP_UPSERT_BATCH, payload);
        }
    }

    void remove(const std::string &resource_guid, bool append_log = true) {
        removeByResourceId(resource_guid);
        if (append_log) {
            Json::Value payload;
            payload["resource_guid"] = resource_guid;
            _log_impl->appendLocalDataMutation(EntityTraits<VmsKvPair>::tableName(), TRAN_DATA_OP_DELETE, payload);
        }
    }

    // Transaction-aware variants — writes run inside an existing transaction.
    void addWithTxn(VmsKvPair &kv, toolkit::SqliteTransaction::Ptr txn) {
        addInternalWithTxn(kv, txn);
    }

    void addBatchWithTxn(const std::vector<VmsKvPair> &kvs, toolkit::SqliteTransaction::Ptr txn) {
        for (auto kv : kvs) {
            addInternalWithTxn(kv, txn);
        }
    }

    void removeWithTxn(const std::string &resource_guid, toolkit::SqliteTransaction::Ptr txn) {
        removeByResourceIdWithTxn(resource_guid, txn);
    }

    std::vector<VmsKvPair> findAllKeyValue(const std::string &resoure_id) {
        return findByResourceId(resoure_id);
    }

    static std::string syncRowKey(const std::string &resource_guid, const std::string &name) {
        return resource_guid + ":" + name;
    }

    static TableSyncHandler makeSyncHandler() {
        TableSyncHandler h;
        // One LWW version per (resource, key), also for every item of an UPSERT_BATCH.
        h.rowKey = [](const Json::Value &p) -> std::string {
            if (p["name"].asString().empty()) return "";
            return syncRowKey(p["resource_guid"].asString(), p["name"].asString());
        };
        // DELETE removes every kv of a resource: the resource is the group.
        h.groupKey = [](const Json::Value &p) -> std::string {
            return p["resource_guid"].asString();
        };
        h.onGroupDeleteWithTxn = [](const Json::Value &p, const std::set<std::string> &keep, toolkit::SqliteTransaction::Ptr txn) {
            const std::string resource_guid = p["resource_guid"].asString();
            if (resource_guid.empty()) return;
            const std::string prefix = syncRowKey(resource_guid, "");
            std::set<std::string> keep_names;
            for (const auto &key : keep) {
                if (key.compare(0, prefix.size(), prefix) == 0) {
                    keep_names.insert(key.substr(prefix.size()));
                }
            }
            std::make_shared<VmsKvPairImp>()->removeByResourceIdExceptWithTxn(resource_guid, keep_names, txn);
        };
        h.onUpsert = [](const Json::Value &p) {
            auto imp = std::make_shared<VmsKvPairImp>();
            auto kv = VmsKvPair::fromJson(p);
            imp->add(kv, false);
        };
        h.onUpsertBatch = [](const Json::Value &p) {
            auto imp = std::make_shared<VmsKvPairImp>();
            std::vector<VmsKvPair> kvs;
            for (const auto &item : p["items"]) {
                kvs.push_back(VmsKvPair::fromJson(item));
            }
            imp->addBatch(kvs, false);
        };
        h.onDelete = [](const Json::Value &p) {
            auto imp = std::make_shared<VmsKvPairImp>();
            std::string resource_guid = p["resource_guid"].asString();
            if (!resource_guid.empty()) imp->remove(resource_guid, false);
        };
        h.onSnapshot = [](const Json::Value &arr) {
            auto imp = std::make_shared<VmsKvPairImp>();
            for (const auto &v : arr) {
                auto kv = VmsKvPair::fromJson(v);
                imp->add(kv, false);
            }
        };
        h.onUpsertWithTxn = [](const Json::Value &p, toolkit::SqliteTransaction::Ptr txn) {
            auto imp = std::make_shared<VmsKvPairImp>();
            auto kv = VmsKvPair::fromJson(p);
            imp->addWithTxn(kv, txn);
        };
        h.onUpsertBatchWithTxn = [](const Json::Value &p, toolkit::SqliteTransaction::Ptr txn) {
            auto imp = std::make_shared<VmsKvPairImp>();
            std::vector<VmsKvPair> kvs;
            for (const auto &item : p["items"]) {
                kvs.push_back(VmsKvPair::fromJson(item));
            }
            imp->addBatchWithTxn(kvs, txn);
        };
        h.onDeleteWithTxn = [](const Json::Value &p, toolkit::SqliteTransaction::Ptr txn) {
            auto imp = std::make_shared<VmsKvPairImp>();
            std::string resource_guid = p["resource_guid"].asString();
            if (!resource_guid.empty()) imp->removeWithTxn(resource_guid, txn);
        };
        return h;
    }

private:
    // Trả về true nếu thực sự ghi DB
    bool addInternal(VmsKvPair &kv) {
        static std::mutex s_add_mtx;
        std::lock_guard<std::mutex> lk(s_add_mtx);
        auto ret = findByResourceIdAndKey(kv.resource_guid, kv.name);
        if (!ret.empty()) {
            kv.id = ret[0].id;
            if (ret[0].value == kv.value) return false;  // ← dirty check
            kv.id = ret[0].id;
            updateById(kv);
            return true;
        } 
        save(kv);
        return true;
    }

    // Transaction-aware internal upsert.
    void addInternalWithTxn(VmsKvPair &kv, toolkit::SqliteTransaction::Ptr txn) {
        static std::mutex s_add_mtx;
        std::lock_guard<std::mutex> lk(s_add_mtx);
        auto ret = findByResourceIdAndKeyWithTxn(kv.resource_guid, kv.name, txn);
        if (!ret.empty()) {
            kv.id = ret[0].id;
            if (ret[0].value == kv.value) return;  // dirty check
            updateByIdWithTxn(kv, txn);
        } else {
            saveWithTxn(kv, txn);
        }
    }

private:
    TransactionLogImp::Ptr _log_impl;
};

} // namespace managerkit

#endif // S3MANAGERKIT_VMSKVPAIR_H
