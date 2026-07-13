#include <gtest/gtest.h>

#include "Common/Stamp.h"

using namespace mediakit;

TEST(DeltaStampTest, ProducesRelativeMonotonicTimeAndResets) {
    DeltaStamp stamp;
    EXPECT_EQ(0, stamp.relativeStamp(1000));
    EXPECT_EQ(40, stamp.relativeStamp(1040));
    EXPECT_EQ(80, stamp.relativeStamp(1080));
    stamp.reset();
    EXPECT_EQ(0, stamp.relativeStamp(5000));
}

TEST(DeltaStampTest, LimitsLargeJumps) {
    DeltaStamp stamp;
    stamp.setMaxDelta(100);
    EXPECT_EQ(0, stamp.relativeStamp(1000));
    const int64_t relative = stamp.relativeStamp(100000);
    EXPECT_GE(relative, 0);
    EXPECT_LE(relative, 100);
}

TEST(StampTest, RevisesAndAllowsSeeking) {
    Stamp stamp;
    int64_t dts = -1;
    int64_t pts = -1;
    stamp.revise(1000, 1040, dts, pts);
    EXPECT_GE(dts, 0);
    EXPECT_GE(pts, dts);

    stamp.setRelativeStamp(5000);
    EXPECT_EQ(5000, stamp.getRelativeStamp());
    stamp.enableRollback(true);
    stamp.setPlayBack(true);
    stamp.reset();
    EXPECT_EQ(0, stamp.getRelativeStamp());
}

TEST(NtpStampTest, ConvertsRtpTicksToMilliseconds) {
    NtpStamp stamp;
    stamp.setNtpStamp(90000, 10000);
    EXPECT_NEAR(11000.0, static_cast<double>(stamp.getNtpStamp(180000, 90000)), 1.0);
    EXPECT_NEAR(9000.0, static_cast<double>(stamp.getNtpStamp(0, 90000)), 1.0);
}
