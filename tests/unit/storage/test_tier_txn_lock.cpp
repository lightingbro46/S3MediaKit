#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <map>
#include <set>
#include <thread>
#include <unistd.h>

#include "Extension/Resource.h"
#include "Storage/DbStorage.h"
#include "Storage/ExtractJob.h"
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

// Regression: a COLD range recorded on an encrypted HOT pool was merged into the adjacent COLD
// range of the unencrypted default pool and lost its origin, so restored segments could not be
// decrypted.
TEST(TierTxnLockTest, MergeNormalizedRangeKeepsDifferentOriginsApart) {
    Exposed<SegmentTierRangeRepository> repo;
    const int64_t base = baseTime();
    auto plain = makeRange("origin-plain", "cam-origin", base, base + 60);
    plain.tier = "COLD";
    plain.pool_id = "pool-cold";
    plain.origin_pool_id = "pool-hot";
    ASSERT_TRUE(repo.save(plain, true));
    auto encrypted = makeRange("origin-enc", "cam-origin", base + 61, base + 120);
    encrypted.tier = "COLD";
    encrypted.pool_id = "pool-cold";
    encrypted.origin_pool_id = "pool-hot-enc";

    ASSERT_TRUE(repo.mergeNormalizedRange(encrypted, 10, now()));
    auto stored_plain = repo.findById(plain);
    ASSERT_EQ(1u, stored_plain.size());
    EXPECT_EQ(base + 60, stored_plain[0].end_time);
    auto stored_enc = repo.findById(encrypted);
    ASSERT_EQ(1u, stored_enc.size());
    EXPECT_EQ("pool-hot-enc", stored_enc[0].origin_pool_id);
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

// beginWriteTxn turns the deferred transaction into an immediate one with "COMMIT; BEGIN IMMEDIATE".
// When the BEGIN IMMEDIATE times out the transaction must still be one the destructor can roll back,
// and the error must be the lock, not a follow-up failure.
TEST(SqliteWriteTxnTest, BeginWriteTxnTimeoutReportsLockAndRecovers) {
    WriteLockHolder lock(3600);  // longer than busy_timeout (3 s)
    auto executor = std::make_shared<SqliteQueryExecutor>(Database::kMediaServerDb);
    std::string error;
    try {
        executor->beginWriteTxn();
    } catch (const std::exception &ex) {
        error = ex.what();
    }
    EXPECT_NE(std::string::npos, error.find("locked")) << error;
    lock.join();

    auto txn = executor->beginWriteTxn();
    txn->execScript("INSERT INTO migrationhistory (app_name, migration, applied) VALUES ('after-timeout', 'after-timeout', 'now');");
    txn->commit();
}

// A statement that fails to prepare (missing table, bad SQL) used to throw
// "basic_string::_M_construct null not valid" instead of the SQLite error.
TEST(SqliteWriteTxnTest, PrepareFailureKeepsSqliteError) {
    auto executor = std::make_shared<SqliteQueryExecutor>(Database::kMediaServerDb);
    std::string error;
    try {
        executor->executeRaw("SELECT * FROM table_that_does_not_exist");
    } catch (const std::exception &ex) {
        error = ex.what();
    }
    EXPECT_NE(std::string::npos, error.find("no such table")) << error;
}

namespace {

RestoreJob makeRestoreJob(const std::string &id, const std::string &status, int64_t updated_at) {
    RestoreJob job;
    job.job_id = id;
    job.camera_id = "cam-restore";
    job.source_tier = "COLD";
    job.target_tier = "HOT";
    job.status = status;
    job.created_at = updated_at;
    job.updated_at = updated_at;
    return job;
}

RestoreJobSegment makeRestoreSegment(const std::string &job_id) {
    RestoreJobSegment segment;
    segment.job_id = job_id;
    segment.camera_id = "cam-restore";
    segment.stream_id = "cam-restore-main";
    segment.range_id = "range-restore";
    segment.segment_path = "2026-10-02/12/00-00.mp4";
    segment.source_pool_id = "pool-cold";
    segment.status = "DONE";
    return segment;
}

} // namespace

TEST(RetentionPruneTest, PrunesOnlyOldExpiredRestoreJobsWithTheirSegments) {
    RestoreJobImp jobs;
    const int64_t cutoff = now() - 100;
    ASSERT_TRUE(jobs.addWithSegments(makeRestoreJob("restore-old-expired", "EXPIRED", cutoff - 1),
                                     {makeRestoreSegment("restore-old-expired")}));
    ASSERT_TRUE(jobs.addWithSegments(makeRestoreJob("restore-new-expired", "EXPIRED", cutoff + 1),
                                     {makeRestoreSegment("restore-new-expired")}));
    ASSERT_TRUE(jobs.addWithSegments(makeRestoreJob("restore-old-done", "DONE", cutoff - 1),
                                     {makeRestoreSegment("restore-old-done")}));

    jobs.pruneExpiredOlderThan(cutoff);

    RestoreJobSegmentImp segments;
    EXPECT_TRUE(jobs.findByJobId("restore-old-expired").empty());
    EXPECT_TRUE(segments.queryByJob("restore-old-expired").empty());
    EXPECT_EQ(1u, jobs.findByJobId("restore-new-expired").size());
    EXPECT_EQ(1u, segments.queryByJob("restore-new-expired").size());
    // DONE jobs may still back a restored cache file.
    EXPECT_EQ(1u, jobs.findByJobId("restore-old-done").size());
    EXPECT_EQ(1u, segments.queryByJob("restore-old-done").size());
}

TEST(RetentionPruneTest, PrunesStorageAlertsOlderThanCutoff) {
    StorageAlertImp alerts;
    const int64_t cutoff = now() - 100;
    StorageAlert alert;
    alert.level = "ERROR";
    alert.type = "TIER_DELETE_FAILED";
    alert.pool_id = "pool-prune";
    alert.message = "test";
    alert.id = "alert-old";
    alert.created_at = cutoff - 1;
    ASSERT_TRUE(alerts.add(alert));
    alert.id = "alert-new";
    alert.created_at = cutoff + 1;
    ASSERT_TRUE(alerts.add(alert));

    alerts.pruneOlderThan(cutoff);

    auto remaining = alerts.query("", -1, 0, 100);
    ASSERT_EQ(1u, remaining.size());
    EXPECT_EQ("alert-new", remaining[0].id);
}

TEST(RetentionPruneTest, PrunesOnlyOldFinishedExtractJobs) {
    ExtractJobImp repo;
    const int64_t cutoff = now() - 100;
    struct Case {
        const char *id;
        const char *status;
        const char *callback_status;
        int64_t updated_at;
        bool kept;
    };
    const Case cases[] = {
        {"extract-success", "SUCCESS", "DELIVERED", cutoff - 1, false},
        {"extract-failed", "FAILED", "DEAD", cutoff - 1, false},
        {"extract-no-callback", "SUCCESS", "NONE", cutoff - 1, false},
        {"extract-recent", "SUCCESS", "DELIVERED", cutoff + 1, true},
        {"extract-callback-pending", "SUCCESS", "PENDING", cutoff - 1, true},
        {"extract-uploading", "UPLOADING", "NONE", cutoff - 1, true},
    };
    for (const auto &c : cases) {
        ExtractJob job;
        job.file_id = c.id;
        job.request_hash = c.id;
        job.camera_id = "cam-extract";
        job.status = c.status;
        job.callback_status = c.callback_status;
        job.created_at = c.updated_at;
        job.updated_at = c.updated_at;
        ExtractJob stored;
        ASSERT_EQ(ExtractJobRepository::CreateResult::CREATED, repo.createOrFind(job, stored));
    }

    repo.pruneFinishedOlderThan(cutoff);

    for (const auto &c : cases)
        EXPECT_EQ(c.kept, !repo.findByFileId(c.id).empty()) << c.id;
}

TEST(RetentionPruneTest, OldestStoredStartSkipsDeletedRangesAndUnfinishedBackfill) {
    Exposed<SegmentTierRangeRepository> repo;
    auto stored = makeRange("oldest-stored", "cam-oldest", 1000, 2000);
    ASSERT_TRUE(repo.save(stored, true));
    auto deleted = makeRange("oldest-deleted", "cam-oldest", 500, 900);
    deleted.status = "DELETED";
    ASSERT_TRUE(repo.save(deleted, true));
    auto sub = makeRange("oldest-sub", "cam-oldest", 1500, 2500);
    sub.stream_id = "cam-oldest-sub";
    ASSERT_TRUE(repo.save(sub, true));
    ASSERT_TRUE(repo.save(makeRange("oldest-backfilling", "cam-oldest-backfill", 100, 200), true));

    TierBackfillTaskRepository backfill;
    TierBackfillTask task;
    task.task_id = "oldest-backfill-task";
    task.camera_id = "cam-oldest-backfill";
    task.timeline_path = "/tmp/oldest-backfill";
    task.status = tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING);
    task.created_at = now();
    task.updated_at = now();
    ASSERT_TRUE(backfill.insertTask(task));

    std::map<std::string, int64_t> oldest;
    for (const auto &r : repo.findOldestStoredStartPerStream()) {
        EXPECT_NE("cam-oldest-backfill", r.camera_id);
        if (r.camera_id == "cam-oldest")
            oldest[r.stream_id] = r.start_time;
    }
    ASSERT_EQ(2u, oldest.size());
    EXPECT_EQ(1000, oldest["cam-oldest-main"]);
    EXPECT_EQ(1500, oldest["cam-oldest-sub"]);
}

TEST(RetentionPruneTest, StatusGapsFollowRecordingCutoffElseFallbackRetention) {
    const uint64_t t = static_cast<uint64_t>(now());
    const uint64_t day = 24 * 3600;
    LocalResourceStatusGapImp gaps;
    auto addGap = [&](const std::string &resource, const std::string &id, uint64_t reconnect) {
        LocalResourceStatusGap gap;
        gap.resource_id = resource;
        gap.gap_id = id;
        gap.disconnect_time = reconnect - 10;
        gap.reconnect_time = reconnect;
        gap.updated_time = reconnect;
        ASSERT_TRUE(gap.reconnect_time > 0 && gaps.addOrUpdate(gap));
    };
    // Covered camera keeping 90 days of recordings: a 40-day-old gap survives.
    addGap("gap-cam-long", "before-cutoff", t - 95 * day);
    addGap("gap-cam-long", "inside-retention", t - 40 * day);
    // Covered camera keeping 2 days: a 5-day-old gap goes.
    addGap("gap-cam-short", "before-cutoff", t - 5 * day);
    addGap("gap-cam-short", "inside-retention", t - day);
    // Uncovered resource: the 30-day fallback applies.
    addGap("gap-cam-unknown", "old", t - 40 * day);
    addGap("gap-cam-unknown", "recent", t - 10 * day);
    // Still offline: never removed.
    LocalResourceStatusGap open;
    open.resource_id = "gap-cam-short";
    open.gap_id = "open";
    open.disconnect_time = t - 50 * day;
    ASSERT_TRUE(gaps.addOrUpdate(open));

    std::unordered_map<std::string, uint64_t> cutoffs;
    cutoffs["gap-cam-long"] = t - 90 * day;
    cutoffs["gap-cam-short"] = t - 2 * day;
    ResourceStatusManager::Instance().setRetentionCutoffs(cutoffs);

    auto ids = [&](const std::string &resource) {
        std::set<std::string> ret;
        for (const auto &gap : gaps.findByResourceIdAndTimeRange(resource))
            ret.insert(gap.gap_id);
        return ret;
    };
    EXPECT_EQ(std::set<std::string>({"inside-retention"}), ids("gap-cam-long"));
    EXPECT_EQ(std::set<std::string>({"inside-retention", "open"}), ids("gap-cam-short"));
    EXPECT_EQ(std::set<std::string>({"recent"}), ids("gap-cam-unknown"));
}
