#include "Common/config.h"
#include "Util/QueryBuilder.h"
#include "Util/util.h"
#include "Util/File.h"
#include "DbStorage.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace Database {
#define DATABASE_FIELD "database."

const string kDbSavePath = DATABASE_FIELD"db_save_path";
const string kMServerMigrationSavePath = DATABASE_FIELD"mserver_updates_save_path";
const string kESCMigrationSavePath = DATABASE_FIELD"updates_save_path";
const string kMediaServerDb = "mserver";
const string kEdgeStorageControllerDb = "esc";

static onceToken token([]() { 
    mINI::Instance()[kDbSavePath] = "./db";
    mINI::Instance()[kMServerMigrationSavePath] = "./mserver_updates";
    mINI::Instance()[kESCMigrationSavePath] = "./updates";
});

} // namespace Database

namespace managerkit {

INSTANCE_IMP(SqlitePoolMap)

SqlitePoolMap::SqlitePoolMap() {
    GET_CONFIG(std::string, dbSavePath, Database::kDbSavePath);
    _save_path = dbSavePath;
    File::create_file(_save_path, "wb+");
}

SqlitePool::Ptr SqlitePoolMap::get(const std::string &tag)  {
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = _pools.find(tag);
    if (it != _pools.end()) {
        return it->second;
    }
    return add(tag);
}

std::string SqlitePoolMap::getSavePath(const std::string &tag) {
    if (_pools.find(tag) != _pools.end()) {
        return _save_path + "/" + tag + ".sqlite";
    }
    return "";
}

SqlitePool::Ptr SqlitePoolMap::add(const std::string &tag) {
    auto db_name = tag + ".sqlite";
    auto full_path = File::absolutePath(db_name, _save_path);
    auto pool = std::make_shared<SqlitePool>();
    pool->Init(full_path);
    pool->setSize(3 + std::thread::hardware_concurrency());
    return _pools.emplace(tag, pool).first->second;
}

} // namespace managerkit
