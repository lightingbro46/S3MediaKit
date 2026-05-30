#ifndef STORAGE_MISC_DATA_H
#define STORAGE_MISC_DATA_H

#include <string>
#include "DbStorage.h"
#include "Util/util.h"
#include "Common/macros.h"

namespace managerkit {

#define MISC_DATA_VERSION_KEY "VERSION"
#define MISC_DATA_DB_INSTANCE_ID_KEY "DB_INSTANCE_ID"
#define MISC_DATA_DB_BOOTSTRAP_DONE "DB_BOOTSTRAP_DONE"

struct MiscData {
    std::string key;
    std::string value;
};

DECLARE_ENTITY_NO_PK(MiscData, "misc_data",
    &MiscData::key, "key", 
    &MiscData::value, "data"
)

class MiscDataRepository : public SqliteRepository<MiscData> {
public:
    MiscDataRepository() : SqliteRepository<MiscData>(Database::kEdgeStorageControllerDb) {}

public:
    std::vector<MiscData> findByKey(const std::string &key) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "key = ?";
        whereParams.push_back(key);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MiscData>::getColumns())
                         .from(EntityTraits<MiscData>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<MiscData> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<MiscData>::fromRow(row));
        }
        return ret;
    }

    bool updateByKey(const MiscData &data) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "key = ?";
        whereParams.push_back(data.key);

        auto cols = EntityTraits<MiscData>::getColumns();  // ["key", "data"]
        auto vals = EntityTraits<MiscData>::getValues(data); 

        auto query = toolkit::QueryBuilder()
                         .update(EntityTraits<MiscData>::tableName())
                         .set({{cols[1], vals[1]}})
                         .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }

    std::vector<MiscData> findAll() {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MiscData>::getColumns())
                         .from(EntityTraits<MiscData>::tableName());
        auto rows = _executor->executeRaw(query);
        std::vector<MiscData> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<MiscData>::fromRow(row));
        }
        return ret;
    }
};

class MiscDataImp : public MiscDataRepository {
public:
    using Ptr = std::shared_ptr<MiscDataImp>;
    MiscDataImp() : MiscDataRepository() {}

    void add(MiscData &entity, bool upsert = true) {
        auto entities = findByKey(entity.key);   
        if (entities.size() == 0) {
            save(entity);
            return;
        }
        if (upsert) {
            updateByKey(entity);
        }
    }

    void initMiscData() {
        // upsert version info
        MiscData version = { .key = MISC_DATA_VERSION_KEY, .value = mediakit::kServerName };
        add(version);
        
        // insert db instance id if not exists (used for identifying different media server instances in sync)
        MiscData db_guid = { .key = MISC_DATA_DB_INSTANCE_ID_KEY, .value = toolkit::format_guid_without_dash(toolkit::makeUuidStr()) };
        add(db_guid, false);

        // insert bootstrap done flag
        MiscData bootstrap_done = { .key = MISC_DATA_DB_BOOTSTRAP_DONE, .value = "0" };
        add(bootstrap_done, false);
    }
}; 

} // namespace managerkit

#endif // STORAGE_MISC_DATA_H