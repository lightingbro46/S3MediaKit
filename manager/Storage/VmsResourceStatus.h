#ifndef S3MANAGERKIT_VMSRESOURCESTATUS_H
#define S3MANAGERKIT_VMSRESOURCESTATUS_H

#include <string>
#include "DbStorage.h"
#include "Util/util.h"
#include "json/json.h"

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
};

class VmsResourceStatusImp : public VmsResourceStatusRepository {
public:
    using Ptr = std::shared_ptr<VmsResourceStatusImp>;
    VmsResourceStatusImp() : VmsResourceStatusRepository() {}

    void add(VmsResourceStatus &resource) {
        auto ret = findByGuid(resource.guid);
        if (ret.size() > 0) {
            updateById(resource);
            return;
        }
        save(resource, true);
    }

    void remove(const std::string &guid) {
        removeByGuid(guid);
    }

    int findStatus(const std::string &guid) {
        auto ret = findByGuid(guid);
        if (ret.size() > 0) {
            return ret[0].status;
        }
        return 0;
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_VMSRESOURCESTATUS_H
