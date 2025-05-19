// #include "Util/logger.h"
// #include "Util/onceToken.h"
// #include "Util/mini.h"
// #include "Util/File.h"
// #include "Common/config.h"
// #include "DbManager.h"

// using namespace std;
// using namespace toolkit;
// using namespace mediakit;

// namespace managerkit {

// namespace xDatabase {
// #define DATABASE_FIELD "xDatabase."
// const string kDbFilename = DATABASE_FIELD"db_filename";
// const string kDbSavePath = DATABASE_FIELD"db_sav_path";
// const string kDbTimeoutSec = DATABASE_FIELD"timeout"; 

// static onceToken token([]() {
//     mINI::Instance()[kDbFilename] = "ecs.sqlite;mserver.sqlite";
//     mINI::Instance()[kDbSavePath] = "./www/db";
//     mINI::Instance()[kDbTimeoutSec] = 0;
// });
// }
// }

// void installDbManager() {
//     // SqlitePool::Instance().Init("./timeline.db");
//     // SqlitePool::Instance().setSize(3);
// }