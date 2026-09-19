#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <thread>
#include <unistd.h>

#include "Storage/DbStorage.h"
#include "Storage/MigrationHistory.h"
#include "Storage/StorageTierExtra.h"
#include "Storage/TierBackfillTask.h"
#include "Storage/TieringJob.h"
#include "Util/mini.h"

using namespace managerkit;
using namespace toolkit;

// Regression: tier transactions that SELECT before they write used deferred transactions, so a
// commit from another connection in between made their first write fail with "database is
// locked". Each test holds the mserver write lock for 300ms while the function under test runs.

namespace {

std::string g_db_dir;

class TierTxnDbEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        char tmpl[] = "/tmp/s3mk_tier_txn_test_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(nullptr, dir);
        g_db_dir = dir;
        mINI::Instance()[Database::kDbSavePath] = g_db_dir;
        std::make_shared<MigrationHistoryImp>(Database::kMediaServerDb)->migrate(S3_MSERVER_MIGRATION_DIR);
    }

    void TearDown() override {
        if (!g_db_dir.empty()) {
            std::string cmd = "rm -rf '" + g_db_dir + "'";
            (void)std::system(cmd.c_str());
        }
    }
};

::testing::Environment *const g_env = ::testing::AddGlobalTestEnvironment(new TierTxnDbEnvironment);

// Holds the mserver write lock from another connection for `hold_ms`. Joins on destruction, so a
// function under test that throws fails the test instead of terminating on a joinable thread.
class WriteLockHolder {
public:
    explicit WriteLockHolder(int hold_ms) {
        auto ready = std::make_shared<std::promise<void>>();
        auto ready_future = ready->get_future();
        _writer = std::thread([hold_ms, ready]() {
            auto executor = std::make_shared<SqliteQueryExecutor>(Database::kMediaServerDb);
            auto txn = executor->beginWriteTxn();
            txn->execScript("INSERT INTO migrationhistory (app_name, migration, applied) VALUES ('lock-test', 'lock-test', 'now');");
            ready->set_value();
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
            txn->commit();
        });
        ready_future.wait();
    }

    ~WriteLockHolder() { join(); }

    void join() {
        if (_writer.joinable())
            _writer.join();
    }

private:
    std::thread _writer;
};

// save()/findById() are protected on the repositories; expose them for seeding and checks.
template <typename Repo>
struct Exposed : Repo {
    using Repo::save;
    using Repo::findById;
};

int64_t now() {
    return static_cast<int64_t>(time(nullptr));
}

SegmentTierRange makeRange(const std::string &id, const std::string &camera, int64_t start, int64_t end) {
    SegmentTierRange range;
    range.range_id = id;
    range.camera_id = camera;
    range.stream_id = camera + "-main";
    range.tier = "HOT";
    range.pool_id = "pool-hot";
    range.status = "AVAILABLE";
    range.start_time = start;
    range.end_time = end;
    range.segment_count = 1;
    range.size_bytes = 100;
    range.created_at = now();
    range.updated_at = now();
    return range;
}

// Noon of the current local day keeps test ranges inside one merge bucket.
int64_t baseTime() {
    time_t t = time(nullptr);
    struct tm local;
    localtime_r(&t, &local);
    local.tm_hour = 12;
    local.tm_min = 0;
    local.tm_sec = 0;
    return static_cast<int64_t>(mktime(&local));
}

} // namespace

TEST(TierTxnLockTest, ClaimPendingTieringJobWaitsForConcurrentWriter) {
    Exposed<TieringJobRepository> repo;
    TieringJob job;
    job.job_id = "lock-tiering-job";
    job.camera_id = "cam-job";
    job.stream_id = "cam-job-main";
    job.range_id = "range-job";
    job.source_tier = "HOT";
    job.target_tier = "WARM";
    job.source_pool_id = "pool-hot";
    job.target_pool_id = "pool-warm";
    job.status = "PENDING";
    job.created_at = now();
    job.updated_at = now();
    ASSERT_TRUE(repo.save(job, true));

    WriteLockHolder writer(300);
    auto claimed = repo.claimPendingAt(10, now());
    writer.join();
    ASSERT_EQ(1u, claimed.size());
    EXPECT_EQ(job.job_id, claimed[0].job_id);
}

TEST(TierTxnLockTest, MergeIfUncoveredWaitsForConcurrentWriter) {
    Exposed<SegmentTierRangeRepository> repo;
    const int64_t base = baseTime();
    auto range = makeRange("lock-merge-uncovered", "cam-merge-uncovered", base, base + 60);

    WriteLockHolder writer(300);
    bool ok = repo.mergeIfUncovered(range, 10, now());
    writer.join();
    EXPECT_TRUE(ok);
    EXPECT_EQ(1u, repo.findById(range).size());
}

TEST(TierTxnLockTest, MergeNormalizedRangeWaitsForConcurrentWriter) {
    Exposed<SegmentTierRangeRepository> repo;
    const int64_t base = baseTime();
    auto first = makeRange("lock-merge-first", "cam-merge", base, base + 60);
    ASSERT_TRUE(repo.save(first, true));
    auto second = makeRange("lock-merge-second", "cam-merge", base + 65, base + 120);

    WriteLockHolder writer(300);
    bool ok = repo.mergeNormalizedRange(second, 10, now());
    writer.join();
    EXPECT_TRUE(ok);
    auto stored = repo.findById(first);
    ASSERT_EQ(1u, stored.size());
    EXPECT_EQ(base + 120, stored[0].end_time);
}

TEST(TierTxnLockTest, NormalizeRangeByIdWaitsForConcurrentWriter) {
    Exposed<SegmentTierRangeRepository> repo;
    const int64_t base = baseTime();
    auto range = makeRange("lock-normalize", "cam-normalize", base, base + 60);
    ASSERT_TRUE(repo.save(range, true));

    WriteLockHolder writer(300);
    bool ok = repo.normalizeRangeById(range.range_id, 10, now());
    writer.join();
    EXPECT_TRUE(ok);
    EXPECT_EQ(1u, repo.findById(range).size());
}

TEST(TierTxnLockTest, ClaimPendingRestoreSegmentWaitsForConcurrentWriter) {
    Exposed<RestoreJobSegmentRepository> repo;
    RestoreJobSegment segment;
    segment.job_id = "lock-restore-job";
    segment.camera_id = "cam-restore";
    segment.stream_id = "cam-restore-main";
    segment.range_id = "range-restore";
    segment.segment_path = "/tmp/lock-restore.mp4";
    segment.source_pool_id = "pool-cold";
    segment.status = "PENDING";
    segment.created_at = now();
    segment.updated_at = now();
    ASSERT_TRUE(repo.save(segment, true));

    WriteLockHolder writer(300);
    auto claimed = repo.claimPendingAt(10, now());
    writer.join();
    ASSERT_EQ(1u, claimed.size());
    EXPECT_EQ(segment.segment_path, claimed[0].segment_path);
}

TEST(TierTxnLockTest, PersistRetentionOverrideWaitsForConcurrentWriter) {
    Exposed<SegmentTierRangeRepository> range_repo;
    const int64_t base = baseTime();
    auto range = makeRange("lock-override", "cam-override", base, base + 60);
    range.status = "EXPIRED";
    ASSERT_TRUE(range_repo.save(range, true));

    SegmentRetentionOverrideRepository repo;
    SegmentRetentionOverride item;
    item.range_id = range.range_id;
    item.extend_until = now() + 3600;
    item.updated_at = now();

    WriteLockHolder writer(300);
    bool ok = repo.persistAndReactivateRange(item);
    writer.join();
    EXPECT_TRUE(ok);
    EXPECT_EQ(1u, repo.findByRange(range.range_id).size());
    auto stored = range_repo.findById(range);
    ASSERT_EQ(1u, stored.size());
    EXPECT_EQ("AVAILABLE", stored[0].status);
}

TEST(TierTxnLockTest, BackfillClaimAndCommitWaitForConcurrentWriter) {
    TierBackfillTaskRepository repo;
    const int64_t base = baseTime();
    TierBackfillTask task;
    task.task_id = "lock-backfill-task";
    task.camera_id = "cam-backfill";
    task.timeline_path = "/tmp/lock-backfill";
    task.window_start = base;
    task.cutoff_time = base + 600;
    task.cursor_time = base;
    task.status = tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING);
    task.next_retry_at = 0;
    task.created_at = now();
    task.updated_at = now();
    ASSERT_TRUE(repo.insertTask(task));

    std::vector<TierBackfillTask> claimed;
    {
        WriteLockHolder writer(300);
        claimed = repo.claimNextDueAt(now());
        writer.join();
    }
    ASSERT_EQ(1u, claimed.size());
    EXPECT_EQ(tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING), claimed[0].status);

    TierBackfillChunk chunk;
    chunk.start_time = base;
    chunk.end_time = base + 60;
    chunk.ranges.push_back(makeRange("lock-backfill-range", "cam-backfill", base, base + 60));

    WriteLockHolder writer(300);
    bool ok = repo.commitChunkAt(claimed[0], chunk,
                                 tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING), 0, now());
    writer.join();
    EXPECT_TRUE(ok);
    auto stored = repo.findByTaskId(task.task_id);
    ASSERT_EQ(1u, stored.size());
    EXPECT_EQ(base + 60, stored[0].cursor_time);
}
