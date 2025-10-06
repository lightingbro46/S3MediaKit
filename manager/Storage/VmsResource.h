#ifndef S3MANAGERKIT_VMSRESOURCE_H
#define S3MANAGERKIT_VMSRESOURCE_H

#include <string>
#include "DbStorage.h"
#include "Util/util.h"

namespace managerkit {

struct VmsResource {
    std::string id;
    std::string guid;
    std::string parent_guid;
    std::string name;
    std::string url;
    std::string xtype_guid;
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

protected:
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
};

class VmsResourceImp : public VmsResourceRepository {
public:
    using Ptr = std::shared_ptr<VmsResourceImp>;
    VmsResourceImp() : VmsResourceRepository() {}

    void add(VmsResource &resource) {
        auto ret = findByGuid(resource.guid);
        if (ret.size() > 0) {
            resource.id = ret[0].id;
            updateById(resource);
            return;
        }
        save(resource);
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_VMSRESOURCE_H
