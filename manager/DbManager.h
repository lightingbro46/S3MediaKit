// #ifndef S3MANAGERKIT_DBMANAGER_H
// #define S3MANAGERKIT_DBMANAGER_H

// #include <string>
// #include <mutex>
// #include "Util/logger.h"
// #include "Util/SqlitePool.h"

// namespace managerkit {
// namespace xDatabase
// {
// extern const std::string kDbFilename;
// extern const std::string kDbSavePath;
// extern const std::string kDbTimeoutSec;
// } // namespace xDatabase


// class DbManager {
// public:
//     typedef enum {
//         type_ecs = 0,
//         type_mserver,
//     } DbOriginType;

//     static DbManager &Instance();

//     toolkit::SqlitePool::Ptr getInstance(DbOriginType dBtype);

// private:
//     std::unordered_map<int, toolkit::SqlitePool::Ptr> _map_db;
//     std::recursive_mutex _mtx_db;
// };

// } // namespace managerkit

// void installDbManager();

// #endif // S3MANAGERKIT_DBMANAGER_H
