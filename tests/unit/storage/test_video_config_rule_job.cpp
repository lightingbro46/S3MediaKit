#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "Storage/DbStorage.h"
#include "Storage/MigrationHistory.h"
#include "Storage/VideoConfigRuleJob.h"
#include "Util/mini.h"

using namespace managerkit;
using namespace toolkit;

// VideoConfigRuleJobImp against a temporary mserver DB built from the real migrations: writes of
// the same row from several threads must not collide, and an older transition never overwrites or
// deletes a newer one.

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
