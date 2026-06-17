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
// Database save paths
extern const std::string kDbSavePath;
// Migration save paths for MediaServer and EdgeStorageController
extern const std::string kMServerMigrationSavePath;
extern const std::string kESCMigrationSavePath;
// Database names
extern const std::string kMediaServerDb;
extern const std::string kEdgeStorageControllerDb;
// Whether to enable sync database
extern const std::string kEnableSyncDb;
// Interval for pull loop, in seconds
extern const std::string kPullIntervalSec;
// Limit for each pull batch, to avoid pulling too much data at once
extern const std::string kBatchLimit;
// Number of peers selected in each gossip round.
// A larger value increases propagation speed and convergence,
// but also generates more network traffic and processing overhead.
// Example:
//   fanout = 1 : slower convergence, minimal network usage.
//   fanout = 3 : balanced for most clusters.
//   fanout = N : broadcast to all peers.
extern const std::string kGossipFanout;
// Number of transactions to keep in the transaction log for gossip synchronization.
extern const std::string kTransactionLogKeepLast;
} // namespace Database

namespace managerkit {

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
        return _pool;
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
    using ExecutorWithTxn = toolkit::QueryExecutor<toolkit::SqliteTransaction, toolkit::SqliteTransactionWriter>;

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

    toolkit::SqliteTransaction::Ptr execTxn() {
        auto pool = _helper->pool();
        return std::make_shared<toolkit::SqliteTransaction>(pool);
    }

    template<typename ...ArgsType>
    bool execDMLWithTxn(toolkit::SqliteTransaction::Ptr pool, ArgsType &&...args) {
        return ExecutorWithTxn::execDML(pool, std::forward<ArgsType>(args)...) > 0;
    }

    template<typename ...ArgsType>
    toolkit::SqlitePool::SqlRetType executeRawWithTxn(toolkit::SqliteTransaction::Ptr pool, ArgsType &&...args)  {
        return ExecutorWithTxn::executeRaw(pool, std::forward<ArgsType>(args)...);
    }

private:
    SqliteHelper::Ptr _helper;
    toolkit::EventPoller::Ptr _poller;
};

template<typename T>
class SqliteRepository { 
public:
    SqliteRepository(const std::string &tag) : _tag(tag) { 
        _executor = std::make_shared<SqliteQueryExecutor>(_tag); 
    }

    virtual ~SqliteRepository() = default;

    std::string getTag() { return _tag; }

protected:
    virtual bool save(const T& obj, bool include_id = false) { 
        auto cols = EntityTraits<T>::getColumns();
        auto vals = EntityTraits<T>::getValues(obj);

        std::vector<std::pair<std::string, std::string>> assignments;
        std::vector<std::string> primaryKeys = EntityTraits<T>::getPrimaryKey();
        for (size_t i = 0; i < cols.size(); ++i) {
            if (!include_id && std::find(primaryKeys.begin(), primaryKeys.end(), cols[i]) != primaryKeys.end()) continue;
            assignments.push_back(std::make_pair(cols[i], vals[i]));
        }

        auto query = toolkit::QueryBuilder()
                        .insertInto(EntityTraits<T>::tableName())
                        .values(assignments);
        return _executor->execDML(query) > 0;
    }

    virtual bool updateById(const T& obj) {
        if (!EntityTraits<T>::hasPrimaryKey()) {
            WarnL << "Trying to update entity without primary key, operation not allowed. Entity type: " << typeid(T).name();
            return false;
        }
        auto cols = EntityTraits<T>::getColumns();
        auto vals = EntityTraits<T>::getValues(obj);

        std::vector<std::pair<std::string, std::string>> assignments;
        std::vector<std::string> primaryKeys = EntityTraits<T>::getPrimaryKey();
        for (size_t i = 0; i < cols.size(); ++i) {
            if (std::find(primaryKeys.begin(), primaryKeys.end(), cols[i]) != primaryKeys.end()) continue;
            assignments.push_back(std::make_pair(cols[i], vals[i]));
        }
        std::ostringstream whereClause;
        for (size_t i = 0; i < primaryKeys.size(); ++i) {
            whereClause << primaryKeys[i] << "= ?";
            if (i + 1 < primaryKeys.size()) whereClause << " AND ";
        }

        auto query = toolkit::QueryBuilder()
                            .update(EntityTraits<T>::tableName())
                            .set(assignments)
                            .where(whereClause.str(), EntityTraits<T>::getPrimaryKeyValue(obj));
        return _executor->execDML(query) > 0;
    } 

    virtual bool removeById(const T& obj) {
        if (!EntityTraits<T>::hasPrimaryKey()) {
            WarnL << "Trying to delete entity without primary key, operation not allowed. Entity type: " << typeid(T).name();
            return false;
        }
        std::vector<std::string> primaryKeys = EntityTraits<T>::getPrimaryKey();
        std::ostringstream whereClause;
        for (size_t i = 0; i < primaryKeys.size(); ++i) {
            whereClause << primaryKeys[i] << "= ?";
            if (i + 1 < primaryKeys.size()) whereClause << " AND ";
        }
        auto query = toolkit::QueryBuilder()
                             .deleteFrom(EntityTraits<T>::tableName())
                             .where(whereClause.str(), EntityTraits<T>::getPrimaryKeyValue(obj));
        return _executor->execDML(query) > 0;
    }

    virtual std::vector<T> findById(const T& obj) {
        if (!EntityTraits<T>::hasPrimaryKey()) {
            WarnL << "Trying to query entity without primary key, operation not allowed. Entity type: " << typeid(T).name();
            return {};
        }
        std::vector<std::string> primaryKeys = EntityTraits<T>::getPrimaryKey();
        std::ostringstream whereClause;
        for (size_t i = 0; i < primaryKeys.size(); ++i) {
            whereClause << primaryKeys[i] << "= ?";
            if (i + 1 < primaryKeys.size()) whereClause << " AND ";
        }
        auto query = toolkit::QueryBuilder()
                             .select(EntityTraits<T>::getColumns())
                             .from(EntityTraits<T>::tableName())
                             .where(whereClause.str(), EntityTraits<T>::getPrimaryKeyValue(obj));
        auto rows = _executor->executeRaw(query);
        std::vector<T> ret;
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<T>::fromRow(row));
        }
        return ret;
    }

    bool isCreated() {
        auto query = toolkit::QueryBuilder()
                         .select({"name"})
                         .from("sqlite_master")
                         .where("type='table' AND name=?", { EntityTraits<T>::tableName() });
        auto rows = _executor->executeRaw(query);
        return !rows.empty();
    }

protected:
    std::string _tag;
    SqliteQueryExecutor::Ptr _executor;
};

} // namespace Database

#endif // S3MEDIAKIT_DBSTORAGE_H