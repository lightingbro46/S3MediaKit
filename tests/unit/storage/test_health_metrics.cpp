#include <gtest/gtest.h>

#include <cstdlib>
#include <unistd.h>

#include "Storage/DbStorage.h"
#include "Storage/HealthMetrics.h"
#include "Storage/MigrationHistory.h"
#include "Util/mini.h"

using namespace managerkit;
using namespace toolkit;

// Health monitor history tables (migration 23) against a temporary mserver DB built from the real
// migrations.

namespace {

std::string g_db_dir;

class HealthMetricsDbEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        char tmpl[] = "/tmp/s3mk_health_metrics_XXXXXX";
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

::testing::Environment *const g_env = ::testing::AddGlobalTestEnvironment(new HealthMetricsDbEnvironment);

} // namespace

TEST(HealthMetricsStore, InsertRangeRssAndPurge) {
    HealthMetricsImp imp;
    for (int i = 0; i < 5; ++i) {
        HealthMetric m;
        m.timestamp = 1000 + i * 60;
        m.overall = i == 2 ? "warning" : "ok";
        m.score = i == 2 ? 90 : 100;
        m.cameras_total = 4;
        m.cameras_online = 3;
        m.poller_delay_max_ms = 1.5f;
        m.proc_rss = i == 3 ? 0 : 1000 + i;   // 0 = not measured, left out of the RSS series
        m.components_json = "{\"host\":\"ok\"}";
        m.webhook_failure_pct = 2.5f;
        m.webhook_queued_max = 30 + i;
        m.webhook_queue_wait_max_ms = 4943;
        ASSERT_TRUE(imp.insertMetric(m));
    }
    auto rows = imp.findByTimeRange(1060, 1180);
    ASSERT_EQ(3u, rows.size());
    EXPECT_EQ(1060, rows[0].timestamp);
    EXPECT_EQ("warning", rows[1].overall);
    EXPECT_EQ(90, rows[1].score);
    EXPECT_EQ(3, rows[1].cameras_online);
    EXPECT_FLOAT_EQ(1.5f, rows[1].poller_delay_max_ms);
    EXPECT_EQ("{\"host\":\"ok\"}", rows[1].components_json);
    EXPECT_FLOAT_EQ(2.5f, rows[1].webhook_failure_pct);
    EXPECT_EQ(32, rows[1].webhook_queued_max);
    EXPECT_EQ(4943, rows[1].webhook_queue_wait_max_ms);
    EXPECT_EQ(5u, imp.findByTimeRange(0, 0).size());

    auto rss = imp.findRssSince(1060);
    ASSERT_EQ(3u, rss.size());
    EXPECT_EQ(1060, rss[0].first);
    EXPECT_EQ(1001, rss[0].second);
    EXPECT_EQ(1240, rss[2].first);

    imp.deleteOlderThan(1120);
    EXPECT_EQ(3u, imp.findByTimeRange(0, 0).size());
}

TEST(HealthMetricsStore, ProcessStartsTellCleanAndUncleanRestarts) {
    ProcessStartImp imp;
    const int64_t now = 10000000;
    // First start: nothing before it.
    int64_t first = imp.recordStart(now - 90000, "");
    ASSERT_GT(first, 0);
    ASSERT_TRUE(imp.setStoppedAt(first, now - 80000));
    // Clean restart, then a crash (no stopped_at), then the start after the crash.
    int64_t second = imp.recordStart(now - 3600, "");
    ASSERT_GT(second, first);
    int64_t third = imp.recordStart(now - 1800, "");
    ASSERT_GT(third, second);

    int restarts = -1, unclean = -1;
    imp.countRestartsSince(now - 86400, restarts, unclean);
    // second (previous run ended cleanly) and third (second never stopped); the first is no restart.
    EXPECT_EQ(2, restarts);
    EXPECT_EQ(1, unclean);
    imp.countRestartsSince(now - 2000, restarts, unclean);
    EXPECT_EQ(1, restarts);
    EXPECT_EQ(1, unclean);

    imp.deleteOlderThan(now - 3600);
    imp.countRestartsSince(0, restarts, unclean);
    EXPECT_EQ(2, restarts);
}

TEST(HealthMetricsStore, StreamBaselinesUpsertAndRemove) {
    StreamBaselineImp imp;
    StreamBaselineRow row;
    row.camera_id = "cam-1";
    row.stream_type = 0;
    row.format = "H264 1920x1080";
    row.fps = 25;
    row.bitrate_kbps = 4000;
    row.learnt_at = 100;
    ASSERT_TRUE(imp.upsert(row));
    row.stream_type = 1;
    row.format = "H264 640x360";
    row.fps = 15;
    ASSERT_TRUE(imp.upsert(row));
    // Learnt again: replaced, not duplicated.
    row.fps = 12;
    row.learnt_at = 200;
    ASSERT_TRUE(imp.upsert(row));

    auto all = imp.findAll();
    ASSERT_EQ(2u, all.size());
    for (const auto &r : all) {
        if (r.stream_type == 1) {
            EXPECT_FLOAT_EQ(12, r.fps);
            EXPECT_EQ(200, r.learnt_at);
            EXPECT_EQ("H264 640x360", r.format);
        } else {
            EXPECT_FLOAT_EQ(25, r.fps);
        }
    }
    EXPECT_TRUE(imp.removeStream("cam-1", 0));
    EXPECT_EQ(1u, imp.findAll().size());
}
