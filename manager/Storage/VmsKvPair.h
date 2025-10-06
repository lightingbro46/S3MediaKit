#ifndef S3MANAGERKIT_VMSKVPAIR_H
#define S3MANAGERKIT_VMSKVPAIR_H

#include <string>
#include "DbStorage.h"
#include "Util/util.h"

namespace managerkit {

struct VmsKvPair {
    std::string id;
    std::string resource_guid;
    std::string name;
    std::string value;
};

DECLARE_ENTITY(VmsKvPair, "vms_resource",
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
};

class VmsKvPairImp : public VmsKvPairRepository {
public:
    using Ptr = std::shared_ptr<VmsKvPairImp>;
    VmsKvPairImp() : VmsKvPairRepository() {}

    void add(VmsKvPair &resource) {
        auto ret = findByResourceId(resource.resource_guid);
        if (ret.size() > 0) {
            resource.id = ret[0].id;
            updateById(resource);
            return;
        }
        save(resource);
    }

    std::vector<VmsKvPair> findAllKeyValue(const std::string &resoure_id) {
        return findByResourceId(resoure_id);
    } 
};

} // namespace managerkit

#endif // S3MANAGERKIT_VMSKVPAIR_H
