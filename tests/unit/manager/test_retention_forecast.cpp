#include <gtest/gtest.h>

#include "Local/RetentionForecast.h"

using namespace managerkit;

namespace {

const int64_t kMiB = 1024ll * 1024ll;
const int64_t kDay = 86400;

// 1 MiB/s = 84.375 GiB a day; 100 days of it on a disk used up to 90%.
RetentionForecastInput oneCamera(int64_t required_days, int64_t committed_days = 0) {
    RetentionForecastInput in;
    in.ingest_valid = true;
    in.ingest_mbps = 1.0f;
    in.usable_percent = 90;
    in.total_bytes = kMiB * kDay * 100;
    in.used_bytes = kMiB * kDay * 40;
    in.short_level = 2;
    RetentionCameraDemand cam;
    cam.camera_id = "cam-a";
    cam.mbps = 1.0f;
    cam.required_sec = required_days * kDay;
    in.cameras.push_back(cam);
    cam.required_sec = committed_days * kDay;
    in.committed.push_back(cam);
    return in;
}

} // namespace

TEST(RetentionForecast, DaysFromUsableAndUsedBytes) {
    auto fc = computeRetentionForecast(oneCamera(30), 1.1f);
    EXPECT_TRUE(fc.valid);
    EXPECT_EQ(1, fc.cameras);
    EXPECT_NEAR(90.0, fc.retention_days_estimate, 0.01);
    EXPECT_NEAR(50.0, fc.estimated_remaining_days, 0.01);
    EXPECT_NEAR(30.0, fc.required_days, 0.01);
    EXPECT_EQ(0, fc.observed);
}

TEST(RetentionForecast, ShortWhenRequirementTimesFactorExceedsUsable) {
    // 85 days needed, 90 usable: fits alone, not with a 1.1 safety factor. Short of the target
    // (configured upper bound) is a warning.
    EXPECT_EQ(0, computeRetentionForecast(oneCamera(85), 1.0f).observed);
    EXPECT_EQ(1, computeRetentionForecast(oneCamera(85), 1.1f).observed);

    // Short of the commitment (keepArchivedMinFor) reports short_level.
    EXPECT_EQ(2, computeRetentionForecast(oneCamera(85, 85), 1.1f).observed);
    auto in = oneCamera(85, 85);
    in.short_level = 1;
    EXPECT_EQ(1, computeRetentionForecast(in, 1.1f).observed);
}

TEST(RetentionForecast, CommitmentFitsWhileTargetDoesNot) {
    auto fc = computeRetentionForecast(oneCamera(180, 30), 1.1f);
    EXPECT_EQ(1, fc.observed);
    EXPECT_EQ(1, fc.committed_cameras);
    EXPECT_NEAR(30.0, fc.committed_days, 0.01);
    EXPECT_NEAR(180.0, fc.required_days, 0.01);
    EXPECT_EQ(fc.committed_bytes, kMiB * kDay * 30);
}

TEST(RetentionForecast, UnknownWithoutRateAndFitsWhenIdle) {
    auto in = oneCamera(85);
    in.ingest_valid = false;
    auto fc = computeRetentionForecast(in, 1.1f);
    EXPECT_EQ(-1, fc.observed);
    EXPECT_LT(fc.retention_days_estimate, 0);
    EXPECT_TRUE(fc.toJson()["retention_days_estimate"].isNull());
    EXPECT_TRUE(fc.toJson()["ingest_mbps"].isNull());

    in = oneCamera(85);
    in.ingest_mbps = 0.0f;
    in.cameras.clear();
    EXPECT_EQ(0, computeRetentionForecast(in, 1.1f).observed);
}

TEST(RetentionForecast, CamerasWithoutRequirementDoNotCount) {
    auto in = oneCamera(0);
    RetentionCameraDemand idle;
    idle.camera_id = "cam-b";
    idle.mbps = 0.0f;
    idle.required_sec = 365 * kDay;
    in.cameras.push_back(idle);
    auto fc = computeRetentionForecast(in, 1.1f);
    EXPECT_EQ(0, fc.cameras);
    EXPECT_EQ(0, fc.required_bytes);
    EXPECT_EQ(0, fc.observed);
}

TEST(RetentionForecast, FullDiskHasNoRemainingDays) {
    auto in = oneCamera(10);
    in.used_bytes = in.total_bytes;
    auto fc = computeRetentionForecast(in, 1.1f);
    EXPECT_EQ(0.0, fc.estimated_remaining_days);
    EXPECT_EQ("normal", fc.toJson()["state"].asString());
}

TEST(RetentionForecastBook, SustainRecoverAndUnknownKeepsState) {
    RetentionForecastBook book;
    auto low = computeRetentionForecast(oneCamera(85, 85), 1.1f);
    auto ok = computeRetentionForecast(oneCamera(10), 1.1f);
    auto unknown = oneCamera(85);
    unknown.ingest_valid = false;
    auto unknown_fc = computeRetentionForecast(unknown, 1.1f);

    EXPECT_FALSE(book.update("record_dir", low, 2, 0, 1000));
    EXPECT_EQ(0, low.level);
    EXPECT_TRUE(book.update("record_dir", low, 2, 0, 2000));
    EXPECT_EQ(2, low.level);

    EXPECT_FALSE(book.update("record_dir", unknown_fc, 2, 0, 3000));
    EXPECT_EQ(2, unknown_fc.level);

    EXPECT_FALSE(book.update("record_dir", ok, 2, 0, 4000));
    EXPECT_TRUE(book.update("record_dir", ok, 2, 0, 5000));
    EXPECT_EQ(0, ok.level);

    RetentionForecast out;
    ASSERT_TRUE(book.get("record_dir", out));
    EXPECT_EQ(0, out.level);
    book.remove("record_dir");
    EXPECT_FALSE(book.get("record_dir", out));
}

namespace {

const double kDayBytes = static_cast<double>(kMiB * kDay);  // 1 MiB/s for one day

RetentionTierStay stay(const std::string &pool, int tier, int64_t days, bool spill,
                       const std::string &action = "MOVE_TO_NEXT_TIER") {
    RetentionTierStay s;
    s.pool_id = pool;
    s.tier = tier;
    s.sec = days * kDay;
    s.spill = spill;
    s.overflow_action = action;
    return s;
}

// 1 MiB/s, 7 days HOT then 30 days WARM, deleted at the end of WARM.
RetentionCameraPlan hotWarm(const std::string &id) {
    RetentionCameraPlan plan;
    plan.camera_id = id;
    plan.mbps = 1.0f;
    plan.stays.push_back(stay("hot", 0, 7, true));
    plan.stays.push_back(stay("warm", 1, 30, false, "DELETE_OLDEST"));
    return plan;
}

} // namespace

TEST(RetentionDemand, EachPoolHoldsItsStay) {
    std::map<std::string, int64_t> usable{{"hot", static_cast<int64_t>(10 * kDayBytes)},
                                          {"warm", static_cast<int64_t>(40 * kDayBytes)}};
    auto d = planRetentionDemand({hotWarm("cam-a")}, usable, 1.0f);
    EXPECT_NEAR(7 * kDayBytes, d["hot"].required_bytes, 1);
    EXPECT_NEAR(30 * kDayBytes, d["warm"].required_bytes, 1);
    EXPECT_EQ(0, d["hot"].short_level);
    EXPECT_EQ(0, d["warm"].short_level);
    EXPECT_FLOAT_EQ(1.0f, d["warm"].inflow_mbps);
}

TEST(RetentionDemand, HotExcessMovesToWarm) {
    // HOT holds 5 of the 7 days: 2 days land on WARM, which still has room.
    std::map<std::string, int64_t> usable{{"hot", static_cast<int64_t>(5 * kDayBytes)},
                                          {"warm", static_cast<int64_t>(40 * kDayBytes)}};
    auto d = planRetentionDemand({hotWarm("cam-a")}, usable, 1.0f);
    EXPECT_EQ(1, d["hot"].short_level);
    EXPECT_EQ("MOVE_TO_NEXT_TIER", d["hot"].overflow_action);
    EXPECT_NEAR(32 * kDayBytes, d["warm"].required_bytes, 1);
    EXPECT_EQ(0, d["warm"].short_level);
}

TEST(RetentionDemand, LastTierShortLosesRecordings) {
    // The 2 days pushed down make WARM (31 days) short: recordings are deleted early.
    std::map<std::string, int64_t> usable{{"hot", static_cast<int64_t>(5 * kDayBytes)},
                                          {"warm", static_cast<int64_t>(31 * kDayBytes)}};
    auto d = planRetentionDemand({hotWarm("cam-a")}, usable, 1.0f);
    EXPECT_EQ(1, d["hot"].short_level);
    EXPECT_EQ(2, d["warm"].short_level);
    EXPECT_EQ("DELETE_OLDEST", d["warm"].overflow_action);
}

TEST(RetentionDemand, SafetyFactorShrinksUsable) {
    std::map<std::string, int64_t> usable{{"hot", static_cast<int64_t>(7.5 * kDayBytes)},
                                          {"warm", static_cast<int64_t>(40 * kDayBytes)}};
    EXPECT_EQ(0, planRetentionDemand({hotWarm("cam-a")}, usable, 1.0f)["hot"].short_level);
    EXPECT_EQ(1, planRetentionDemand({hotWarm("cam-a")}, usable, 1.1f)["hot"].short_level);
}

TEST(RetentionDemand, UnknownCapacityTakesEverything) {
    std::map<std::string, int64_t> usable{{"hot", static_cast<int64_t>(5 * kDayBytes)}};
    auto d = planRetentionDemand({hotWarm("cam-a")}, usable, 1.0f);
    EXPECT_EQ(1, d["hot"].short_level);
    EXPECT_NEAR(32 * kDayBytes, d["warm"].required_bytes, 1);
    EXPECT_EQ(0, d["warm"].short_level);
}

TEST(RetentionDemand, ExcessSplitsByShare) {
    // Two cameras on one HOT pool holding 10 of 14 days: each pushes its half of the 4-day excess.
    // cam-b deletes instead of moving, so its share is lost.
    auto a = hotWarm("cam-a");
    RetentionCameraPlan b;
    b.camera_id = "cam-b";
    b.mbps = 1.0f;
    b.stays.push_back(stay("hot", 0, 7, false, "DELETE_OLDEST"));
    std::map<std::string, int64_t> usable{{"hot", static_cast<int64_t>(10 * kDayBytes)},
                                          {"warm", static_cast<int64_t>(40 * kDayBytes)}};
    auto d = planRetentionDemand({a, b}, usable, 1.0f);
    EXPECT_EQ(2, d["hot"].short_level);
    EXPECT_EQ("DELETE_OLDEST", d["hot"].overflow_action);
    EXPECT_NEAR(32 * kDayBytes, d["warm"].required_bytes, 1);
    EXPECT_EQ(1, static_cast<int>(d["warm"].camera_bytes.size()));
}

TEST(RetentionDemand, OpenEndedStayHasNoRequirement) {
    RetentionCameraPlan plan;
    plan.camera_id = "cam-a";
    plan.mbps = 1.0f;
    plan.stays.push_back(stay("hot", 0, 0, false, "STOP_RECORDING_AND_ALERT"));
    std::map<std::string, int64_t> usable{{"hot", static_cast<int64_t>(kDayBytes)}};
    auto d = planRetentionDemand({plan}, usable, 1.0f);
    EXPECT_EQ(0, d["hot"].required_bytes);
    EXPECT_EQ(0, d["hot"].short_level);
}
