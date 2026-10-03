#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

#include "Local/PoolThroughputTracker.h"

using namespace managerkit;

namespace {

const uint64_t kMiB = 1024ull * 1024ull;
const int64_t kSec = 1000;

PoolThroughputConfig testConfig() {
    PoolThroughputConfig cfg;
    cfg.window_sec = 600;
    cfg.safety_factor = 1.2f;
    cfg.min_busy_sec = 60;
    cfg.sustain_samples = 2;
    cfg.alert_repeat_sec = 3600;
    return cfg;
}

// One 10 MiB segment every 10 s = 1 MiB/s, over [from, to).
void feedRecording(PoolThroughputTracker &t, const std::string &camera, int64_t from_sec, int64_t to_sec) {
    for (int64_t s = from_sec; s < to_sec; s += 10) {
        t.addIngest(camera, "hot", 10 * kMiB, s * kSec);
    }
}

void transfer(PoolThroughputTracker &t, const std::string &pool, uint64_t mib, int64_t from_sec, int64_t to_sec) {
    t.beginTransfer(pool, PoolThroughputTracker::WRITE, from_sec * kSec);
    t.endTransfer(pool, PoolThroughputTracker::WRITE, mib * kMiB, to_sec * kSec);
}

std::map<std::string, std::set<std::string>> routeTo(const std::string &pool, const std::string &camera) {
    std::map<std::string, std::set<std::string>> routes;
    routes[pool].insert(camera);
    return routes;
}

} // namespace

TEST(PoolThroughputTracker, ParallelTransfersCountBusyTimeOnce) {
    PoolThroughputTracker t;
    t.beginTransfer("warm", PoolThroughputTracker::WRITE, 0);
    t.beginTransfer("warm", PoolThroughputTracker::WRITE, 50 * kSec);
    t.endTransfer("warm", PoolThroughputTracker::WRITE, 100 * kMiB, 100 * kSec);
    t.endTransfer("warm", PoolThroughputTracker::WRITE, 100 * kMiB, 150 * kSec);

    t.evaluate(std::map<std::string, int64_t>(), testConfig(), 300 * kSec);
    PoolThroughputStats st;
    ASSERT_TRUE(t.getStats("warm", st));
    EXPECT_TRUE(st.write_valid);
    EXPECT_EQ(200 * kMiB, st.write_bytes);
    EXPECT_NEAR(200.0 / 150.0, st.write_mbps, 0.01);
    EXPECT_NEAR(50.0, st.write_busy_pct, 0.1);
    EXPECT_NEAR(200.0 / 300.0, st.avg_write_mbps, 0.01);
    EXPECT_FALSE(st.demand_valid);
}

TEST(PoolThroughputTracker, SlowPoolAlertsAfterSustainAndRecovers) {
    PoolThroughputTracker t;
    const auto cfg = testConfig();
    std::map<std::string, int64_t> backlog;
    backlog["warm"] = static_cast<int64_t>(1000 * kMiB);
    t.setRoutes(routeTo("warm", "cam1"));

    feedRecording(t, "cam1", 0, 620);
    transfer(t, "warm", 150, 100, 400); // 0.5 MiB/s against 1 MiB/s recorded

    EXPECT_TRUE(t.evaluate(backlog, cfg, 600 * kSec).empty());
    auto alerts = t.evaluate(backlog, cfg, 610 * kSec);
    ASSERT_EQ(1u, alerts.size());
    const auto &low = alerts[0];
    EXPECT_EQ("warm", low.stats.pool_id);
    EXPECT_TRUE(low.stats.low);
    EXPECT_NEAR(0.5, low.stats.write_mbps, 0.01);
    EXPECT_NEAR(1.0, low.stats.demand_mbps, 0.05);
    EXPECT_NEAR(1.2, low.required_mbps, 0.06);
    EXPECT_EQ(1, low.stats.demand_cameras);
    EXPECT_NEAR(2000, low.stats.backlog_eta_sec, 1);

    PoolThroughputStats st;
    ASSERT_TRUE(t.getStats("hot", st));
    EXPECT_NEAR(1.0, st.record_ingest_mbps, 0.05);

    // The slow sample leaves the window and the pool now drains at 5 MiB/s.
    feedRecording(t, "cam1", 620, 1320);
    transfer(t, "warm", 500, 750, 850);
    EXPECT_TRUE(t.evaluate(backlog, cfg, 1300 * kSec).empty());
    alerts = t.evaluate(backlog, cfg, 1310 * kSec);
    ASSERT_EQ(1u, alerts.size());
    EXPECT_FALSE(alerts[0].stats.low);
    EXPECT_NEAR(5.0, alerts[0].stats.write_mbps, 0.01);
}

TEST(PoolThroughputTracker, EmptyBacklogIsNotLow) {
    PoolThroughputTracker t;
    const auto cfg = testConfig();
    t.setRoutes(routeTo("warm", "cam1"));
    feedRecording(t, "cam1", 0, 620);
    // Slow, but it drained everything it was given.
    transfer(t, "warm", 60, 0, 120);

    const std::map<std::string, int64_t> no_backlog;
    EXPECT_TRUE(t.evaluate(no_backlog, cfg, 600 * kSec).empty());
    EXPECT_TRUE(t.evaluate(no_backlog, cfg, 610 * kSec).empty());
    PoolThroughputStats st;
    ASSERT_TRUE(t.getStats("warm", st));
    EXPECT_FALSE(st.low);
}

TEST(PoolThroughputTracker, BacklogWithoutTransfersKeepsState) {
    PoolThroughputTracker t;
    const auto cfg = testConfig();
    std::map<std::string, int64_t> backlog;
    backlog["warm"] = static_cast<int64_t>(100 * kMiB);
    t.setRoutes(routeTo("warm", "cam1"));
    feedRecording(t, "cam1", 0, 620);
    transfer(t, "warm", 50, 100, 200); // 0.5 MiB/s

    t.evaluate(backlog, cfg, 600 * kSec);
    ASSERT_EQ(1u, t.evaluate(backlog, cfg, 610 * kSec).size());

    // No transfer in the window any more (pool offline): neither a repeat nor a recovery.
    feedRecording(t, "cam1", 620, 1300);
    EXPECT_TRUE(t.evaluate(backlog, cfg, 1290 * kSec).empty());
    EXPECT_TRUE(t.evaluate(backlog, cfg, 1300 * kSec).empty());
    PoolThroughputStats st;
    ASSERT_TRUE(t.getStats("warm", st));
    EXPECT_FALSE(st.write_valid);
    EXPECT_TRUE(st.low);
    EXPECT_EQ(-1, st.backlog_eta_sec);
}

TEST(PoolThroughputTracker, DemandNeedsObservationTime) {
    PoolThroughputTracker t;
    const auto cfg = testConfig();
    std::map<std::string, int64_t> backlog;
    backlog["warm"] = static_cast<int64_t>(100 * kMiB);
    t.setRoutes(routeTo("warm", "cam1"));
    feedRecording(t, "cam1", 0, 300);
    transfer(t, "warm", 30, 0, 100);

    for (int64_t s = 200; s < 600; s += 100) {
        EXPECT_TRUE(t.evaluate(backlog, cfg, s * kSec).empty());
    }
    PoolThroughputStats st;
    ASSERT_TRUE(t.getStats("warm", st));
    EXPECT_FALSE(st.demand_valid);
    EXPECT_FALSE(st.low);
}

TEST(PoolThroughputTracker, RemovedPoolIsForgotten) {
    PoolThroughputTracker t;
    t.setRoutes(routeTo("warm", "cam1"));
    transfer(t, "warm", 100, 0, 100);
    t.evaluate(std::map<std::string, int64_t>(), testConfig(), 200 * kSec);
    PoolThroughputStats st;
    ASSERT_TRUE(t.getStats("warm", st));

    t.removePool("warm");
    EXPECT_FALSE(t.getStats("warm", st));
    t.evaluate(std::map<std::string, int64_t>(), testConfig(), 300 * kSec);
    EXPECT_FALSE(t.getStats("warm", st));
}

TEST(PoolTransferScope, UnfinishedScopeCountsAsFailedTransfer) {
    PoolThroughputTracker t;
    {
        PoolTransferScope scope(t, "cold", "hot");
    }
    {
        PoolTransferScope scope(t, "cold", "hot");
        scope.finish(5 * kMiB);
    }
    t.evaluate(std::map<std::string, int64_t>(), testConfig(), steadyNowMs() + 1000);
    PoolThroughputStats cold, hot;
    ASSERT_TRUE(t.getStats("cold", cold));
    ASSERT_TRUE(t.getStats("hot", hot));
    EXPECT_EQ(5 * kMiB, cold.write_bytes);
    EXPECT_EQ(5 * kMiB, hot.read_bytes);
}

TEST(PoolThroughputTracker, IngestRatesPerCameraOfOnePool) {
    PoolThroughputTracker t;
    feedRecording(t, "cam-a", 0, 1200);
    for (int64_t s = 0; s < 1200; s += 10) {
        t.addIngest("cam-b", "hot", 5 * kMiB, s * kSec);
        t.addIngest("cam-c", "hot-2", 10 * kMiB, s * kSec);
    }

    auto rates = t.ingestRates("hot", 600, 1200 * kSec);
    EXPECT_TRUE(rates.valid);
    EXPECT_NEAR(1.5, rates.mbps, 0.05);
    ASSERT_EQ(2u, rates.camera_mbps.size());
    EXPECT_NEAR(1.0, rates.camera_mbps["cam-a"], 0.05);
    EXPECT_NEAR(0.5, rates.camera_mbps["cam-b"], 0.05);

    EXPECT_FALSE(t.ingestRates("missing", 600, 1200 * kSec).camera_mbps.size());
}

TEST(PoolThroughputTracker, IngestRatesNeedObservationTimeAndForgetIdleCameras) {
    PoolThroughputTracker t;
    feedRecording(t, "cam-a", 0, 300);
    // Window 3600 s: valid only after min(window, 15 min) of observation.
    EXPECT_FALSE(t.ingestRates("hot", 3600, 300 * kSec).valid);
    auto rates = t.ingestRates("hot", 3600, 1000 * kSec);
    EXPECT_TRUE(rates.valid);
    EXPECT_NEAR(300.0 / 1000.0, rates.mbps, 0.01);

    // Nothing recorded for longer than the window: the camera drops out.
    rates = t.ingestRates("hot", 600, 2000 * kSec);
    EXPECT_TRUE(rates.camera_mbps.empty());
    EXPECT_EQ(0u, rates.bytes);
}
