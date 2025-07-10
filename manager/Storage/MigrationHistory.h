#ifndef STORAGE_MIGRATIONHISTORY_H
#define STORAGE_MIGRATIONHISTORY_H

#include <string>
#include "DbStorage.h"

namespace managerkit {

struct MigrateHistory {
    int id;
    std::string app_name;
    std::string migration;
    std::string applied;
};

DECLARE_ENTITY(MigrateHistory, "migrationHistory",
    { "id" },
    &MigrateHistory::id, "id", 
    &MigrateHistory::app_name, "app_name", 
    &MigrateHistory::migration, "migration", 
    &MigrateHistory::applied, "applied"
)

class MigrationHistoryRepository : public SqliteRepository<MigrateHistory> {
public:
    MigrationHistoryRepository(const std::string &tag) : SqliteRepository<MigrateHistory>(tag) {}
    
    std::vector<MigrateHistory> findByMigration(const std::string path) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MigrateHistory>::getColumns())
                         .from(EntityTraits<MigrateHistory>::tableName())
                         .where("migration = ?", { serialize_sql_value(path) });
        auto rows =  _executor->executeRaw(query);
        std::vector<MigrateHistory> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<MigrateHistory>::fromRow(row));
        }
        return ret;
    }
};

class MigrationHistoryImp : public MigrationHistoryRepository {
public:
    using Ptr = std::shared_ptr<MigrationHistoryImp>;

    MigrationHistoryImp(const std::string &tag) : MigrationHistoryRepository(tag) { }

    ~MigrationHistoryImp();

    void migrate(const std::string &files_string);

private:
    void removeFile(const std::string &folder_path);

    void execSqlQuery(const std::string &sql_query);

private:
    std::mutex _mtx;
    std::string _folder_path;
};

} // namespace managerkit

#endif // STORAGE_MIGRATIONHISTORY_H