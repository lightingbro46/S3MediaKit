#include <string>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Common/config.h"
#include "DbManager.h"

using namespace std;

namespace managerkit {

namespace Database {
#define DATABASE_FIELD "database."
const string krdbms = DATABASE_FIELD"rdbms";
const string kDbHost = DATABASE_FIELD"host";
const string kDbPort = DATABASE_FIELD"port";
const string kDbUser = DATABASE_FIELD"user";
const string kDbPasswd = DATABASE_FIELD"passwd";
const string kDbName = DATABASE_FIELD"dbName";
const string kDbFilename = DATABASE_FIELD"filename";
const string kDbTimeoutSec = DATABASE_FIELD"timeout"; 

static onceToken token([]() {
    mINI::Instance()[krdbms] = "sqlite3";
    mINI::Instance()[kDbHost] = "localhost";
    mINI::Instance()[kDbPort] = 3306;
    mINI::Instance()[kDbUser] = "";
    mINI::Instance()[kDbPasswd] = "";
    mINI::Instance()[kDbName] = "ecs;mserver";
    mINI::Instance()[kDbFilename] = "./db/ecs.sqlite;./db/mserver.sqlite";
    mINI::Instance()[kDbTimeoutSec] = 0;
});




}
}