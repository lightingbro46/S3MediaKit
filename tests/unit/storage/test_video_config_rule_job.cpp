#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "Storage/DbStorage.h"
#include "Storage/MigrationHistory.h"
#include "Storage/VideoConfigRuleJob.h"
#include "Storage/VideoConfigRuleResult.h"
#include "Camera/VideoConfigJobManager.h"
#include "Common/config.h"
#include "Util/NoticeCenter.h"
#include "Util/mini.h"

using namespace managerkit;
using namespace toolkit;

// VideoConfigRuleJobImp / VideoConfigRuleResultImp against a temporary mserver DB built from the
// real migrations: writes of the same row from several threads must not collide, an older transition
// never overwrites or deletes a newer one, and results are stored per (logId, deviceId).

namespace {

std::string g_db_dir;

class VideoConfigRuleJobDbEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        char tmpl[] = "/tmp/s3mk_video_rule_job_test_XXXXXX";
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

::testing::Environment *const g_env = ::testing::AddGlobalTestEnvironment(new VideoConfigRuleJobDbEnvironment);

VideoConfigRuleJob makeJob(const std::string &device, int64_t updatedAt, int width) {
    VideoConfigRuleJob job;
    job.deviceId = device;
    job.profileToken = "profile_1";
    job.status = (int)RuleJobStatus::ACTIVE;
    job.id = VideoConfigRuleJob::makeId(job.deviceId, job.profileToken, RuleJobStatus::ACTIVE);
    job.logId = "log-" + std::to_string(updatedAt);
    job.vcodec = "H264";
    job.width = width;
    job.height = 720;
    job.fps = 15;
    job.bitrate = 1024;
    job.receivedAt = updatedAt;
    job.deadlineAt = updatedAt + 60000;
    job.updatedAt = updatedAt;
    return job;
}

} // namespace

TEST(VideoConfigRuleJobImpTest, ConcurrentFirstWritesOfOneRowDoNotCollide) {
    const int kThreads = 8;
    auto start = std::make_shared<std::promise<void>>();
    std::shared_future<void> go = start->get_future().share();
    std::atomic<int> failures(0);
    std::vector<std::thread> writers;
    for (int i = 0; i < kThreads; ++i) {
        writers.emplace_back([i, go, &failures]() {
            VideoConfigRuleJobImp repo;
            go.wait();
            try {
                // false only means a newer row was already stored.
                (void)repo.addOrUpdate(makeJob("cam-concurrent", 1000 + i, 640 + i));
            } catch (const std::exception &ex) {
                ADD_FAILURE() << "writer " << i << " threw: " << ex.what();
                ++failures;
            }
        });
    }
    start->set_value();
    for (auto &t : writers) {
        t.join();
    }
    EXPECT_EQ(0, failures.load());

    VideoConfigRuleJobImp repo;
    auto rows = repo.findByIdStr(makeJob("cam-concurrent", 0, 0).id);
    ASSERT_EQ(1u, rows.size());
    // Whatever the interleaving, the newest transition is the one left in the table.
    EXPECT_EQ(1000 + kThreads - 1, rows[0].updatedAt);
    EXPECT_EQ(640 + kThreads - 1, rows[0].width);
}

TEST(VideoConfigRuleJobImpTest, OlderWriteDoesNotOverwriteNewerRow) {
    VideoConfigRuleJobImp repo;
    ASSERT_TRUE(repo.addOrUpdate(makeJob("cam-order", 2000, 1280)));
    EXPECT_FALSE(repo.addOrUpdate(makeJob("cam-order", 1000, 640)));

    auto rows = repo.findByIdStr(makeJob("cam-order", 0, 0).id);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(2000, rows[0].updatedAt);
    EXPECT_EQ(1280, rows[0].width);
}

TEST(VideoConfigRuleJobImpTest, NewerOrEqualWriteUpdatesTheRow) {
    VideoConfigRuleJobImp repo;
    ASSERT_TRUE(repo.addOrUpdate(makeJob("cam-update", 1000, 640)));
    EXPECT_TRUE(repo.addOrUpdate(makeJob("cam-update", 1000, 800)));
    EXPECT_TRUE(repo.addOrUpdate(makeJob("cam-update", 3000, 1920)));

    auto rows = repo.findByIdStr(makeJob("cam-update", 0, 0).id);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(3000, rows[0].updatedAt);
    EXPECT_EQ(1920, rows[0].width);
}

TEST(VideoConfigRuleJobImpTest, RemoveKeepsARowUpdatedAfterTheTransition) {
    VideoConfigRuleJobImp repo;
    auto job = makeJob("cam-remove", 5000, 640);
    ASSERT_TRUE(repo.addOrUpdate(job));

    EXPECT_FALSE(repo.removeIfNotNewer(job.id, 4000));
    EXPECT_EQ(1u, repo.findByIdStr(job.id).size());

    EXPECT_TRUE(repo.removeIfNotNewer(job.id, 5000));
    EXPECT_TRUE(repo.findByIdStr(job.id).empty());

    // Nothing left to remove is not an error.
    EXPECT_TRUE(repo.removeIfNotNewer(job.id, 6000));
}

TEST(VideoConfigRuleResultImpTest, PendingRowTakesTheResultAndKeepsItsCreationTime) {
    VideoConfigRuleResultImp repo;
    ASSERT_TRUE(repo.markPending("log-a", "cam-2", 1000));
    ASSERT_TRUE(repo.markPending("log-a", "cam-1", 1000));
    ASSERT_TRUE(repo.markPending("log-other", "cam-1", 1000));

    auto rows = repo.findByLogId("log-a");
    ASSERT_EQ(2u, rows.size());
    EXPECT_EQ("cam-1", rows[0].deviceId);
    EXPECT_EQ("cam-2", rows[1].deviceId);
    EXPECT_EQ(VideoConfigRuleStatus::kPending, rows[0].status);

    ASSERT_TRUE(repo.recordResult("log-a", "cam-1", false, "CONFIG_NOT_APPLIED", "kept the old config", 5000));
    ASSERT_TRUE(repo.recordResult("log-a", "cam-2", true, "ignored", "ignored", 6000));
    rows = repo.findByLogId("log-a");
    ASSERT_EQ(2u, rows.size());
    EXPECT_EQ(VideoConfigRuleStatus::kFailed, rows[0].status);
    EXPECT_EQ("CONFIG_NOT_APPLIED", rows[0].errCode);
    EXPECT_EQ("kept the old config", rows[0].errMsg);
    EXPECT_EQ(1000, rows[0].createdAt);
    EXPECT_EQ(5000, rows[0].updatedAt);
    EXPECT_EQ(VideoConfigRuleStatus::kSuccess, rows[1].status);
    EXPECT_EQ("", rows[1].errCode);

    // A request reusing the logId starts the camera over.
    ASSERT_TRUE(repo.markPending("log-a", "cam-1", 7000));
    rows = repo.findByLogId("log-a");
    EXPECT_EQ(VideoConfigRuleStatus::kPending, rows[0].status);
    EXPECT_EQ("", rows[0].errCode);
    EXPECT_EQ(7000, rows[0].createdAt);
}

TEST(VideoConfigRuleResultImpTest, ResultWithoutPendingRowIsCreated) {
    VideoConfigRuleResultImp repo;
    ASSERT_TRUE(repo.recordResult("log-recovered", "cam-1", true, "", "", 3000));
    auto rows = repo.findByLogId("log-recovered");
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(VideoConfigRuleStatus::kSuccess, rows[0].status);
    EXPECT_EQ(3000, rows[0].createdAt);
}

TEST(VideoConfigRuleResultImpTest, PruneRemovesRowsNotUpdatedSinceTheCutoff) {
    VideoConfigRuleResultImp repo;
    ASSERT_TRUE(repo.markPending("log-prune", "cam-old", 1000));
    ASSERT_TRUE(repo.markPending("log-prune", "cam-new", 9000));
    repo.removeUpdatedBefore(5000);
    auto rows = repo.findByLogId("log-prune");
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ("cam-new", rows[0].deviceId);
}

TEST(VideoConfigRuleResultImpTest, PendingRowsAndReportedStateOfOneCamera) {
    VideoConfigRuleResultImp repo;
    ASSERT_TRUE(repo.markPending("log-p2", "cam-state", 1000));
    ASSERT_TRUE(repo.markPending("log-p1", "cam-state", 1000));
    ASSERT_TRUE(repo.markPending("log-done", "cam-state", 1000));
    ASSERT_TRUE(repo.markPending("log-p1", "cam-other", 1000));
    ASSERT_TRUE(repo.recordResult("log-done", "cam-state", false, "RULE_EXPIRED", "expired", 2000));

    auto pending = repo.findPendingByDeviceId("cam-state");
    ASSERT_EQ(2u, pending.size());
    EXPECT_EQ("log-p1", pending[0].logId);
    EXPECT_EQ("log-p2", pending[1].logId);

    bool success = true;
    EXPECT_FALSE(repo.isReported("log-p1", "cam-state", success));
    EXPECT_FALSE(repo.isReported("log-unknown", "cam-state", success));
    EXPECT_TRUE(repo.isReported("log-done", "cam-state", success));
    EXPECT_FALSE(success);
}

TEST(VideoConfigRuleJobImpTest, RemoveByDeviceIdKeepsOtherCameras) {
    VideoConfigRuleJobImp repo;
    auto active = makeJob("cam-gone", 1000, 640);
    auto queued = active;
    queued.status = (int)RuleJobStatus::QUEUED;
    queued.id = VideoConfigRuleJob::makeId(queued.deviceId, queued.profileToken, RuleJobStatus::QUEUED);
    auto other = makeJob("cam-stays", 1000, 640);
    ASSERT_TRUE(repo.addOrUpdate(active));
    ASSERT_TRUE(repo.addOrUpdate(queued));
    ASSERT_TRUE(repo.addOrUpdate(other));

    repo.removeByDeviceId("cam-gone");
    EXPECT_TRUE(repo.findDeviceJobs("cam-gone", "profile_1").empty());
    EXPECT_EQ(1u, repo.findDeviceJobs("cam-stays", "profile_1").size());
}

// Contract of the receiver of the recording-encryption fork (/api/media-server/action-callback).
TEST(VideoConfigRuleResultTest, HookBodyMatchesTheForkContract) {
    VideoConfigRuleResult ok;
    ok.logId = "log-1";
    ok.deviceId = "cam-1";
    ok.status = VideoConfigRuleStatus::kSuccess;
    ok.errCode = "leftover";
    auto body = ok.toHookJson();
    EXPECT_EQ("log-1", body["logId"].asString());
    EXPECT_EQ("cam-1", body["deviceId"].asString());
    EXPECT_EQ("SUCCESS", body["status"].asString());
    EXPECT_TRUE(body.isMember("errorCode"));
    EXPECT_TRUE(body["errorCode"].isNull());
    EXPECT_TRUE(body["errorMessage"].isNull());
    EXPECT_EQ(5u, body.size());

    VideoConfigRuleResult failed = ok;
    failed.status = VideoConfigRuleStatus::kFailed;
    failed.errCode = "RULE_EXPIRED";
    failed.errMsg = "expired";
    body = failed.toHookJson();
    EXPECT_EQ("FAILED", body["status"].asString());
    EXPECT_EQ("RULE_EXPIRED", body["errorCode"].asString());
    EXPECT_EQ("expired", body["errorMessage"].asString());
}

namespace {

std::vector<VideoConfigRuleResult> queryResults(const std::string &logId) {
    auto done = std::make_shared<std::promise<std::vector<VideoConfigRuleResult>>>();
    auto future = done->get_future();
    VideoConfigJobManager::Instance().queryResults(logId,
        [done](const std::vector<VideoConfigRuleResult> &rows, const std::string &error) {
            EXPECT_EQ("", error);
            done->set_value(rows);
        });
    EXPECT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds(5)));
    return future.get();
}

} // namespace

// The API queues the PENDING rows, then the cameras report on their pollers: every result lands on
// its row, and a lookup queued afterwards sees them.
TEST(VideoConfigJobManagerResultTest, ResultsFollowTheAcceptedRowsInOrder) {
    auto &manager = VideoConfigJobManager::Instance();
    manager.start();

    manager.recordAccepted("log-m", { "cam-a", "cam-b", "cam-c" });
    VideoConfigJobManager::reportFailure("log-m", "cam-b", "DEVICE_OFFLINE", "Device is offline");
    const std::string none;
    NOTICE_EMIT(BroadcastVideoConfigRuleResultArgs, mediakit::Broadcast::kBroadcastVideoConfigRuleResult,
                std::string("log-m"), std::string("cam-a"), true, none, none);

    auto rows = queryResults("log-m");
    manager.stop();
    ASSERT_EQ(3u, rows.size());
    EXPECT_EQ(VideoConfigRuleStatus::kSuccess, rows[0].status);
    EXPECT_EQ(VideoConfigRuleStatus::kFailed, rows[1].status);
    EXPECT_EQ("DEVICE_OFFLINE", rows[1].errCode);
    EXPECT_EQ(VideoConfigRuleStatus::kPending, rows[2].status);

    // Stopped: results are no longer recorded.
    NOTICE_EMIT(BroadcastVideoConfigRuleResultArgs, mediakit::Broadcast::kBroadcastVideoConfigRuleResult,
                std::string("log-m"), std::string("cam-c"), true, none, none);
    rows = queryResults("log-m");
    ASSERT_EQ(3u, rows.size());
    EXPECT_EQ(VideoConfigRuleStatus::kPending, rows[2].status);
}

// A camera deleted while its requests are pending: each of them fails with DEVICE_REMOVED, requests
// already answered keep their result, and the camera's job rows are gone.
TEST(VideoConfigJobManagerResultTest, RemovedCameraFailsItsPendingRequestsAndDropsItsJobs) {
    auto &manager = VideoConfigJobManager::Instance();
    manager.start();

    {
        VideoConfigRuleJobImp jobs;
        ASSERT_TRUE(jobs.addOrUpdate(makeJob("cam-del", 1000, 640)));
    }
    manager.recordAccepted("log-del-1", { "cam-del", "cam-keep" });
    manager.recordAccepted("log-del-2", { "cam-del" });
    manager.recordAccepted("log-del-3", { "cam-del" });
    const std::string none;
    NOTICE_EMIT(BroadcastVideoConfigRuleResultArgs, mediakit::Broadcast::kBroadcastVideoConfigRuleResult,
                std::string("log-del-3"), std::string("cam-del"), true, none, none);

    manager.removeDevice("cam-del");

    // Each query also waits for the writes queued before it.
    auto first = queryResults("log-del-1");
    auto second = queryResults("log-del-2");
    auto third = queryResults("log-del-3");
    manager.stop();

    ASSERT_EQ(2u, first.size());
    EXPECT_EQ("cam-del", first[0].deviceId);
    EXPECT_EQ(VideoConfigRuleStatus::kFailed, first[0].status);
    EXPECT_EQ("DEVICE_REMOVED", first[0].errCode);
    EXPECT_EQ(VideoConfigRuleStatus::kPending, first[1].status); // cam-keep untouched
    ASSERT_EQ(1u, second.size());
    EXPECT_EQ("DEVICE_REMOVED", second[0].errCode);
    ASSERT_EQ(1u, third.size());
    EXPECT_EQ(VideoConfigRuleStatus::kSuccess, third[0].status);

    VideoConfigRuleJobImp jobs;
    EXPECT_TRUE(jobs.findDeviceJobs("cam-del", "profile_1").empty());
}
