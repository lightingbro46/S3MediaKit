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
const string kEnableSyncDb = DATABASE_FIELD"enable_sync_db";
const string kPullIntervalSec = DATABASE_FIELD"pull_interval_sec";
const string kBatchLimit = DATABASE_FIELD"batch_limit";
const string kGossipFanout = DATABASE_FIELD"gossip_fanout";

static onceToken token([]() { 
    mINI::Instance()[kDbSavePath] = "./db";
    mINI::Instance()[kMServerMigrationSavePath] = "./mserver_updates";
    mINI::Instance()[kESCMigrationSavePath] = "./updates";
    mINI::Instance()[kEnableSyncDb] = true;
    mINI::Instance()[kPullIntervalSec] = 30;
    mINI::Instance()[kBatchLimit] = 100;
    mINI::Instance()[kGossipFanout] = 3;
});

} // namespace Database

namespace managerkit {

INSTANCE_IMP(SqlitePoolMap)

SqlitePoolMap::SqlitePoolMap() {
    GET_CONFIG(std::string, dbSavePath, Database::kDbSavePath);
    GET_CONFIG(std::string, mediaServerId, General::kMediaServerId);
    _save_path = dbSavePath;
    // Make saving directory and try to create file to check write permission
    auto file_check = File::absolutePath(mediaServerId + ".txt", _save_path);
    shared_ptr<FILE>(File::create_file(file_check, "wb+"), [](FILE *fp) {
        if (fp) {
            fclose(fp);
        }
    });
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
