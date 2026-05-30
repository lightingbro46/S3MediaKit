#ifndef S3MANAGERKIT_LOCALRESOURCE_H
#define S3MANAGERKIT_LOCALRESOURCE_H

#include <string>
#include "DbStorage.h"
#include "Util/util.h"

namespace managerkit {

struct LocalResource {
    std::string id;
    std::string resource_id;
    std::string property_name;
    std::string property_value;
};

DECLARE_ENTITY(LocalResource, "local_resource_properties",
    {"id"},
    &LocalResource::id, "id", 
    &LocalResource::resource_id, "resource_id", 
    &LocalResource::property_name, "property_name", 
    &LocalResource::property_value, "property_value"
)

class LocalResourceRepository : public SqliteRepository<LocalResource> {
public:
    LocalResourceRepository() : SqliteRepository<LocalResource>(Database::kMediaServerDb) {}

protected:
    std::vector<LocalResource> findByPropertyName(const std::string &resource_id, const std::string &property_name) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_id = ? AND property_name = ?";
        whereParams.push_back(resource_id);
        whereParams.push_back(property_name);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<LocalResource>::getColumns())
                         .from(EntityTraits<LocalResource>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<LocalResource> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<LocalResource>::fromRow(row));
        }
        return ret;
    }

    std::vector<LocalResource> findByResourceId(const std::string &resource_id) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_id = ?";
        whereParams.push_back(resource_id);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<LocalResource>::getColumns())
                         .from(EntityTraits<LocalResource>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<LocalResource> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<LocalResource>::fromRow(row));
        }
        return ret;
    }

    bool removeByResourceId(const std::string &resource_id) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_id = ?";
        whereParams.push_back(resource_id);

        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<LocalResource>::tableName())
                            .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }
};

class LocalResourceImp : public LocalResourceRepository {
public:
    using Ptr = std::shared_ptr<LocalResourceImp>;
    LocalResourceImp() : LocalResourceRepository() {}

    // Single upsert với dirty check
    bool add(LocalResource &resource) {
        auto ret = findByPropertyName(resource.resource_id, resource.property_name);
        if (!ret.empty()) {
            if (ret[0].property_value == resource.property_value) return false; // ← dirty check
            resource.id = ret[0].id;
            updateById(resource);
            return true;
        }
        save(resource);
        return true;
    }

    // Batch upsert trong 1 SQLite transaction (không có transaction log)
    void addBatch(std::vector<LocalResource> &props) {
        if (props.empty()) return;
        for (auto &prop : props) {
            add(prop);   // dirty check bên trong
        }
    }

    void remove(const std::string &resource_id) {
        removeByResourceId(resource_id);
    }

    std::vector<LocalResource> findAllProperty(const std::string &resoure_id) {
        return findByResourceId(resoure_id);
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_LOCALRESOURCE_H
