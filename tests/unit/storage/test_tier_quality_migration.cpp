#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include <sqlite3.h>

namespace {

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str());
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

void exec(sqlite3 *db, const std::string &sql) {
    char *error = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error))
        << (error ? error : "unknown sqlite error");
    sqlite3_free(error);
}

} // namespace

TEST(TierQualityMigrationTest, PreservesLegacyJobsWithCopyDefaults) {
    char path[] = "/tmp/s3mk_tier_quality_XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    sqlite3 *db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(path, &db));

    exec(db,
        "CREATE TABLE tiering_jobs ("
        "job_id TEXT PRIMARY KEY,camera_id TEXT,stream_id TEXT,range_id TEXT,"
        "source_tier TEXT,target_tier TEXT,source_pool_id TEXT,target_pool_id TEXT,"
        "status TEXT,segment_start_time INTEGER,segment_end_time INTEGER,"
        "bytes_total INTEGER,bytes_moved INTEGER,error_message TEXT,created_at INTEGER,"
        "updated_at INTEGER,job_trigger TEXT,policy_id TEXT,policy_updated_at INTEGER);"
        "INSERT INTO tiering_jobs VALUES ('legacy','cam','main','range','HOT','WARM',"
        "'hot','warm','PENDING',1,2,100,0,NULL,3,3,'LEGACY','',0);");
    exec(db, readFile(S3_TIER_QUALITY_MIGRATION));

    sqlite3_stmt *statement = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db,
        "SELECT operation,quality_profile_json,output_bytes,transcoded_segment_count,"
        "preserved_segment_count,encoder_summary_json FROM tiering_jobs WHERE job_id='legacy'",
        -1, &statement, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(statement));
    EXPECT_STREQ("COPY_MOVE", reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
    EXPECT_STREQ("{}", reinterpret_cast<const char *>(sqlite3_column_text(statement, 1)));
    EXPECT_EQ(0, sqlite3_column_int64(statement, 2));
    EXPECT_EQ(0, sqlite3_column_int64(statement, 3));
    EXPECT_EQ(0, sqlite3_column_int64(statement, 4));
    EXPECT_STREQ("{}", reinterpret_cast<const char *>(sqlite3_column_text(statement, 5)));
    sqlite3_finalize(statement);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
        "('tiering_job_segments','tier_timefile_size_updates')", -1, &statement, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(statement));
    EXPECT_EQ(2, sqlite3_column_int(statement, 0));
    sqlite3_finalize(statement);
    sqlite3_close(db);
    unlink(path);
}
