#include <algorithm>
#include "Common/macros.h"
#include "Util/File.h"
#include "Util/QueryBuilder.h"
#include "MigrationHistory.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

MigrationHistoryImp::~MigrationHistoryImp() {
    removeFile(_folder_path);
}

void MigrationHistoryImp::migrate(const string &files_string) {
    std::lock_guard<std::mutex> lock(_mtx);

    std::vector<std::string> files;
    if (File::is_dir(files_string)) {
        _folder_path = files_string;
        File::scanDir(files_string, [&](const string &path, bool is_dir) {
            if (!is_dir && end_with(path, ".sql")) {
                files.emplace_back(path);
            }
            return true;
        });
        std::sort(files.begin(), files.end());
    } else {
        files = split(files_string, ";");
    }

    bool check_sql_file = true;
    std::string file_save_path = SqlitePoolMap::Instance().getSavePath(getTag());
    file_save_path = File::absolutePath("", file_save_path);
    if (File::fileSize(file_save_path) == 0) {
        check_sql_file = false;
    }

    for (const auto &file : files) {
        if (File::fileExist(file)) {
            try {
                if (check_sql_file) {
                    auto records = findByMigration(file);
                    if (!records.empty()) {
                        continue;
                    }
                }
                DebugL << "Consider to execute sql file: " << file;
                auto sql_stmt = File::loadFile(file);
                execSqlQuery(sql_stmt);
                DebugL << "Execute sql statement success: " << file;
                MigrateHistory entry;
                entry.app_name = mediakit::kServerName;
                entry.migration = file;
                entry.applied = getTimeStr("%Y-%m-%dT%H:%M:%S");
                save(entry);
                TraceL << "Insert migrate history entry success: " << entry.migration;
            } catch (SqliteException &ex) {
                WarnL << "MigrationHistoryImp::migrate failed: " << ex.what();
            }
        }
    }
}

bool MigrationHistoryImp::execSqlQuery(string &sql_query) {
    return _executor->execDML(sql_query);
}

void MigrationHistoryImp::removeFile(const std::string &folder_path) {
    File::scanDir(folder_path, [](const string &path, bool isDir) {
        TraceL << "Consider to delete file: " << path;
        File::delete_file(path, true, false);
        TraceL << "Delete file success: " << path;
        return true;
    });
}

} // namespace managerkit
