#include <gtest/gtest.h>

#include "Local/RetentionForecast.h"

using namespace managerkit;

namespace {

const int64_t kMiB = 1024ll * 1024ll;
const int64_t kDay = 86400;

// 1 MiB/s = 84.375 GiB a day; 100 days of it on a disk used up to 90%.
RetentionForecastInput oneCamera(int64_t required_days) {
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
    // 85 days needed, 90 usable: fits alone, not with a 1.1 safety factor.
    EXPECT_EQ(0, computeRetentionForecast(oneCamera(85), 1.0f).observed);
    EXPECT_EQ(2, computeRetentionForecast(oneCamera(85), 1.1f).observed);

    auto in = oneCamera(85);
    in.short_level = 1;
    EXPECT_EQ(1, computeRetentionForecast(in, 1.1f).observed);
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
    auto low = computeRetentionForecast(oneCamera(85), 1.1f);
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
