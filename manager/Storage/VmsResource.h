#ifndef S3MANAGERKIT_VMSRESOURCE_H
#define S3MANAGERKIT_VMSRESOURCE_H

#include <string>
#include <json/json.h>
#include "DbStorage.h"
#include "Util/util.h"
#include "TransactionLog.h"
#include "Extension/TableSyncHandler.h"

namespace managerkit {

struct VmsResource {
    std::string id;
    std::string guid;
    std::string parent_guid;
    std::string name;
    std::string url;
    std::string xtype_guid;

    Json::Value toJson() const {
        Json::Value v;
        v["id"]          = id;
        v["guid"]        = guid;
        v["parent_guid"] = parent_guid;
        v["name"]        = name;
        v["url"]         = url;
        v["xtype_guid"]  = xtype_guid;
        return v;
    }

    static VmsResource fromJson(const Json::Value &v) {
        VmsResource r;
        r.id          = v["id"].asString();
        r.guid        = v["guid"].asString();
        r.parent_guid = v["parent_guid"].asString();
        r.name        = v["name"].asString();
        r.url         = v["url"].asString();
        r.xtype_guid  = v["xtype_guid"].asString();
        return r;
    }
};

DECLARE_ENTITY(VmsResource, "vms_resource",
    {"id"},
    &VmsResource::id, "id", 
    &VmsResource::guid, "guid", 
    &VmsResource::parent_guid, "parent_guid", 
    &VmsResource::name, "name",
    &VmsResource::url, "url",
    &VmsResource::xtype_guid, "xtype_guid"
)

class VmsResourceRepository : public SqliteRepository<VmsResource> {
public:
    VmsResourceRepository() : SqliteRepository<VmsResource>(Database::kEdgeStorageControllerDb) {}

    std::vector<VmsResource> findByGuid(const std::string &guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "guid = ?";
        whereParams.push_back(guid);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<VmsResource>::getColumns())
                         .from(EntityTraits<VmsResource>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<VmsResource> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VmsResource>::fromRow(row));
        }
        return ret;
    }

    bool removeByGuid(const std::string &guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "guid = ?";
        whereParams.push_back(guid);

        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<VmsResource>::tableName())
                            .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }

    std::vector<VmsResource> findByParentGuidAndXType(const std::string &guid, const std::string &xtype_guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "parent_guid = ? AND xtype_guid = ?";
        whereParams.push_back(guid);
        whereParams.push_back(xtype_guid);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<VmsResource>::getColumns())
                         .from(EntityTraits<VmsResource>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<VmsResource> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VmsResource>::fromRow(row));
        }
        return ret;
    }
};

class VmsResourceImp : public VmsResourceRepository {
public:
    using Ptr = std::shared_ptr<VmsResourceImp>;
    VmsResourceImp() : VmsResourceRepository() {
        _log_impl = std::make_shared<TransactionLogImp>();
    }

    void add(VmsResource &resource, bool append_log = true) {
        auto ret = findByGuid(resource.guid);
        bool changed = false;
        if (ret.size() > 0) {
            resource.id = ret[0].id;
            if (!equal(resource, ret[0])) {
                updateById(resource);
                changed = true;
            }
        } else {
            save(resource);
            changed = true;
        }
        if (changed && append_log) {
            _log_impl->appendLocalDataMutation(EntityTraits<VmsResource>::tableName(), TRAN_DATA_OP_UPSERT, resource.toJson());
        }
    }

    void remove(const std::string &guid, bool append_log = true) {
        removeByGuid(guid);
        if (append_log) {
            Json::Value payload;
            payload["guid"] = guid;
            _log_impl->appendLocalDataMutation(EntityTraits<VmsResource>::tableName(), TRAN_DATA_OP_DELETE, payload);
        }
    }

    static TableSyncHandler makeSyncHandler() {
        TableSyncHandler h;
        h.rowKey = [](const Json::Value &p) -> std::string {
            return p["guid"].asString();
        };
        h.onUpsert = [](const Json::Value &p) {
            auto imp = std::make_shared<VmsResourceImp>();
            auto r = VmsResource::fromJson(p);
            imp->add(r, false);
        };
        h.onUpsertBatch = nullptr;
        h.onDelete = [](const Json::Value &p) {
            auto imp = std::make_shared<VmsResourceImp>();
            std::string guid = p["guid"].asString();
            if (!guid.empty()) imp->remove(guid, false);
        };
        h.onSnapshot = [](const Json::Value &arr) {
            auto imp = std::make_shared<VmsResourceImp>();
            for (const auto &v : arr) {
                auto r = VmsResource::fromJson(v);
                imp->add(r, false);
            }
        };
        return h;
    }

private:
    bool equal(const VmsResource &r1, const VmsResource &r2) {
        return r1.parent_guid == r2.parent_guid
            && r1.name == r2.name
            && r1.url == r2.url
            && r1.xtype_guid == r2.xtype_guid;
    }

private:
    TransactionLogImp::Ptr _log_impl;
};

} // namespace managerkit

#endif // S3MANAGERKIT_VMSRESOURCE_H
