#ifndef S3MANAGERKIT_VMSRESOURCETYPE_H
#define S3MANAGERKIT_VMSRESOURCETYPE_H

#include <string>
#include "DbStorage.h"
#include "Util/util.h"

namespace managerkit {

struct VmsResourceType {
    std::string id;
    std::string name;
    Optional<std::string> description;
    Optional<int> manufacture_id;
    std::string guid;
};

DECLARE_ENTITY(VmsResourceType, "vms_resourcetype",
    {"id"},
    &VmsResourceType::id, "id", 
    &VmsResourceType::name, "name",
    &VmsResourceType::description, "description",
    &VmsResourceType::manufacture_id, "manufacture_id",
    &VmsResourceType::guid, "guid"
)

class VmsResourceTypeRepository : public SqliteRepository<VmsResourceType> {
public:
    VmsResourceTypeRepository() : SqliteRepository<VmsResourceType>(Database::kEdgeStorageControllerDb) {}

public:
    std::vector<VmsResourceType> findAll() {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<VmsResourceType>::getColumns())
                         .from(EntityTraits<VmsResourceType>::tableName());
        auto rows = _executor->executeRaw(query);
        std::vector<VmsResourceType> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VmsResourceType>::fromRow(row));
        }
        return ret;
    }
};

class VmsResourceTypeImp : public VmsResourceTypeRepository {
public:
    using Ptr = std::shared_ptr<VmsResourceTypeImp>;
    VmsResourceTypeImp() : VmsResourceTypeRepository() {}
};

} // namespace managerkit

#endif // S3MANAGERKIT_VMSRESOURCETYPE_H
