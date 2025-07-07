#ifndef S3MEDIAKIT_DBSTORAGE_H
#define S3MEDIAKIT_DBSTORAGE_H

#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include "Poller/EventPoller.h"
#include "Util/SqlitePool.h"
#include "Util/QueryBuilder.h"
#include "DbSchema.h"

namespace Database {

extern const std::string kDbSavePath;
extern const std::string kMServerMigrationSavePath;
extern const std::string kESCMigrationSavePath;

extern const std::string kMediaServerDb;
extern const std::string kEdgeStorageControllerDb;

} // namespace Database

namespace managerkit {

static bool is_migrate_db = false;

// Global Sqlite pool record object, convenient for later management
// Thread-safe
class SqlitePoolMap : public std::enable_shared_from_this<SqlitePoolMap> {
public:
    using Ptr = std::shared_ptr<SqlitePoolMap>;

    static SqlitePoolMap &Instance();
    ~SqlitePoolMap() = default;

    toolkit::SqlitePool::Ptr get(const std::string &tag);

    std::string getSavePath(const std::string &tag);
    
private:
    SqlitePoolMap();

    toolkit::SqlitePool::Ptr add(const std::string &tag);

private:
    std::mutex _mtx;
    std::string _save_path;
    std::unordered_map<std::string, toolkit::SqlitePool::Ptr> _pools;
};

class SqliteHelper {
public: 
    using Ptr = std::shared_ptr<SqliteHelper>;

    SqliteHelper(const std::string &tag) {
        _tag = std::move(tag);
        //Get the pool in the global map for easy management later
        _pool_map = SqlitePoolMap::Instance().shared_from_this();
        _pool =_pool_map->get(tag);
    }

    ~SqliteHelper() = default;

    toolkit::SqlitePool::Ptr pool() {
        return is_migrate_db ? nullptr : _pool;
    }

private:
    std::string _tag;
    toolkit::SqlitePool::Ptr _pool;
    SqlitePoolMap::Ptr _pool_map;
};

class SqliteQueryExecutor {
public:
    using Ptr = std::shared_ptr<SqliteQueryExecutor>;
    using Executor = toolkit::QueryExecutor<toolkit::SqlitePool, toolkit::SqliteBaseWriter>;

    SqliteQueryExecutor(const std::string &tag, toolkit::EventPoller::Ptr poller = nullptr) {
        _poller = poller ? std::move(poller) : toolkit::EventPollerPool::Instance().getPoller();
        _helper = std::make_shared<SqliteHelper>(tag);
    }

    template<typename ...ArgsType>
    bool execDML(ArgsType &&...args) {
        auto pool = _helper->pool();
        return Executor::execDML(pool, std::forward<ArgsType>(args)...) > 0;
    }

    template<typename ...ArgsType>
    toolkit::SqlitePool::SqlRetType executeRaw(ArgsType &&...args)  {
        auto pool = _helper->pool();
        return Executor::executeRaw(pool, std::forward<ArgsType>(args)...);
    }

private:
    SqliteHelper::Ptr _helper;
    toolkit::EventPoller::Ptr _poller;
};

template<typename T>
class SqliteRespository { 
public:
    SqliteRespository(const std::string &tag) { 
        _executor = std::make_shared<SqliteQueryExecutor>(tag); 
    }

    virtual ~SqliteRespository() = default;

    virtual bool save(const T& obj) { 
        auto cols = EntityTraits<T>::getColumns();
        auto vals = EntityTraits<T>::getValues(obj);

        std::vector<std::pair<std::string, std::string>> assignments;
        for (size_t i = 0; i < cols.size(); ++i) {
            if (cols[i] == EntityTraits<T>::getPrimaryKey()) continue;
            assignments.push_back(std::make_pair(cols[i], vals[i]));
        }

        auto query = toolkit::QueryBuilder()
                        .insertInto(EntityTraits<T>::tableName())
                        .values(assignments);
        return _executor->execDML(query);
    }

    virtual bool updateById(const T& obj) {
        auto cols = EntityTraits<T>::getColumns();
        auto vals = EntityTraits<T>::getValues(obj);

        std::vector<std::pair<std::string, std::string>> assignments;
        for (size_t i = 0; i < cols.size(); ++i) {
            if (cols[i] == EntityTraits<T>::getPrimaryKey()) continue;
            assignments.push_back(std::make_pair(cols[i], vals[i]));
        }

        auto query = toolkit::QueryBuilder()
                            .update(EntityTraits<T>::tableName())
                            .set(assignments)
                            .where(EntityTraits<T>::getPrimaryKey() + "= ?", { EntityTraits<T>::getPrimaryKeyValue(obj) });
        return _executor->execDML(query);
    } 

    virtual bool removeById(const T& obj) {
        auto query = toolkit::QueryBuilder()
                             .deleteFrom(EntityTraits<T>::tableName())
                             .where(EntityTraits<T>::getPrimaryKey(), { EntityTraits<T>::getPrimaryKeyValue(obj) });
        return _executor->execDML(query);
    }

protected:
    SqliteQueryExecutor::Ptr _executor;
};

} // namespace Database

#endif // S3MEDIAKIT_DBSTORAGE_H