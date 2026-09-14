#include <gtest/gtest.h>

#include <limits>

#ifdef ENABLE_MOTION
#include "Local/MotionRangeUtils.h"

using managerkit::MotionTimeRange;

TEST(MotionSearchTest, FiltersMotionByRecordedRangesAndPreservesRecordingGaps) {
    const std::vector<MotionTimeRange> motions = {{110, 40}};
    const std::vector<MotionTimeRange> recordings = {{140, 20}, {100, 20}};

    const auto result = managerkit::MotionRangeUtils::filterByRecordedRanges(motions, recordings);

    ASSERT_EQ(2u, result.size());
    EXPECT_EQ(110u, result[0].startTime);
    EXPECT_EQ(10u, result[0].duration);
    EXPECT_EQ(140u, result[1].startTime);
    EXPECT_EQ(10u, result[1].duration);
}

TEST(MotionSearchTest, DropsMotionWhenNoRecordedRangeExists) {
    const std::vector<MotionTimeRange> motions = {{100, 20}};

    EXPECT_TRUE(managerkit::MotionRangeUtils::filterByRecordedRanges(motions, {}).empty());
}

TEST(MotionSearchTest, ClipsSortsAndMergesFilteredMotion) {
    const std::vector<MotionTimeRange> motions = {{145, 15}, {90, 30}, {120, 30}};
    const std::vector<MotionTimeRange> recordings = {{100, 25}, {125, 25}};

    const auto result = managerkit::MotionRangeUtils::filterByRecordedRanges(motions, recordings);

    ASSERT_EQ(1u, result.size());
    EXPECT_EQ(100u, result[0].startTime);
    EXPECT_EQ(50u, result[0].duration);
}

TEST(MotionSearchTest, ExcludesRangesThatOnlyTouchAtBoundary) {
    const std::vector<MotionTimeRange> motions = {{80, 20}, {120, 20}};
    const std::vector<MotionTimeRange> recordings = {{100, 20}};

    EXPECT_TRUE(managerkit::MotionRangeUtils::filterByRecordedRanges(motions, recordings).empty());
}

TEST(MotionSearchTest, HandlesRangesNearMaximumTimestampWithoutOverflow) {
    const uint64_t max_time = std::numeric_limits<uint64_t>::max();
    const std::vector<MotionTimeRange> motions = {{max_time - 10, 10}};
    const std::vector<MotionTimeRange> recordings = {{max_time - 5, 5}};

    const auto result = managerkit::MotionRangeUtils::filterByRecordedRanges(motions, recordings);

    ASSERT_EQ(1u, result.size());
    EXPECT_EQ(max_time - 5, result[0].startTime);
    EXPECT_EQ(5u, result[0].duration);
}
#endif
