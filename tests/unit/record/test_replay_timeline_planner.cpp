#include <gtest/gtest.h>

#include "Record/ReplayTimelinePlanner.h"

using namespace mediakit;

namespace {

ReplayBlock block(int source, uint64_t start, uint64_t end, const char *path) {
    ReplayBlock value;
    value.source = source;
    value.stream_id = source == 0 ? "main" : "sub";
    value.file_path = path;
    value.start_time = start;
    value.end_time = end;
    value.file_start_time = start;
    return value;
}

} // namespace

TEST(ReplayTimelinePlannerTest, MapsPublicQualityAndPreferredToInternalModes) {
    EXPECT_EQ(ReplayMode::OnlyHigh, ReplayTimelinePlanner::resolveMode("hi", "lo"));
    EXPECT_EQ(ReplayMode::OnlyLow, ReplayTimelinePlanner::resolveMode("lo", "hi"));
    EXPECT_EQ(ReplayMode::AutoHigh, ReplayTimelinePlanner::resolveMode("auto", "hi"));
    EXPECT_EQ(ReplayMode::AutoLow, ReplayTimelinePlanner::resolveMode("auto", "lo"));
}

TEST(ReplayTimelinePlannerTest, PreferredStreamWinsOnlyTheOverlap) {
    std::vector<ReplayBlock> blocks;
    blocks.push_back(block(1, 100, 140, "sub.mp4"));
    blocks.push_back(block(0, 110, 130, "main.mp4"));

    const auto clips = ReplayTimelinePlanner::build(blocks, 100, 140, ReplayMode::AutoHigh);
    ASSERT_EQ(3u, clips.size());
    EXPECT_EQ(1, clips[0].source);
    EXPECT_EQ(100u, clips[0].start_time);
    EXPECT_EQ(10u, clips[0].duration);
    EXPECT_EQ(0, clips[1].source);
    EXPECT_EQ(110u, clips[1].start_time);
    EXPECT_EQ(20u, clips[1].duration);
    EXPECT_EQ(1, clips[2].source);
    EXPECT_EQ(130u, clips[2].start_time);
    EXPECT_EQ(10u, clips[2].duration);

    const auto low_clips = ReplayTimelinePlanner::build(blocks, 100, 140, ReplayMode::AutoLow);
    ASSERT_EQ(1u, low_clips.size());
    EXPECT_EQ(1, low_clips[0].source);
    EXPECT_EQ(100u, low_clips[0].start_time);
    EXPECT_EQ(40u, low_clips[0].duration);
}

TEST(ReplayTimelinePlannerTest, OnlyModeDoesNotFallbackToOtherStream) {
    std::vector<ReplayBlock> blocks;
    blocks.push_back(block(1, 100, 140, "sub.mp4"));

    EXPECT_TRUE(ReplayTimelinePlanner::build(blocks, 100, 140, ReplayMode::OnlyHigh).empty());
    EXPECT_FALSE(ReplayTimelinePlanner::build(blocks, 100, 140, ReplayMode::OnlyLow).empty());
}

TEST(ReplayTimelinePlannerTest, StopsAtGapLongerThanTheLimit) {
    std::vector<ReplayBlock> blocks;
    blocks.push_back(block(1, 100, 110, "first.mp4"));
    blocks.push_back(block(1, 120, 130, "later.mp4"));

    const auto clips = ReplayTimelinePlanner::build(blocks, 100, 130, ReplayMode::AutoLow);
    ASSERT_EQ(1u, clips.size());
    EXPECT_EQ("first.mp4", clips.front().file_path);
    EXPECT_EQ(10u, clips.front().duration);
}

TEST(ReplayTimelinePlannerTest, RequiresTheRequestedTimestampToBeCovered) {
    std::vector<ReplayBlock> blocks;
    blocks.push_back(block(0, 105, 120, "later.mp4"));

    EXPECT_TRUE(ReplayTimelinePlanner::build(blocks, 100, 120, ReplayMode::AutoHigh).empty());
}

TEST(ReplayTimelinePlannerTest, BridgesShortGapBetweenSegments) {
    std::vector<ReplayBlock> blocks;
    blocks.push_back(block(0, 100, 110, "first.mp4"));
    blocks.push_back(block(0, 111, 120, "second.mp4"));

    const auto clips = ReplayTimelinePlanner::build(blocks, 100, 120, ReplayMode::AutoHigh);
    ASSERT_EQ(2u, clips.size());
    EXPECT_EQ("first.mp4", clips[0].file_path);
    EXPECT_EQ(10u, clips[0].duration);
    EXPECT_EQ("second.mp4", clips[1].file_path);
    EXPECT_EQ(111u, clips[1].start_time);
    EXPECT_EQ(9u, clips[1].duration);
}

TEST(ReplayTimelinePlannerTest, StopsAtThreeSecondGap) {
    std::vector<ReplayBlock> blocks;
    blocks.push_back(block(0, 100, 110, "first.mp4"));
    blocks.push_back(block(0, 113, 120, "second.mp4"));

    const auto clips = ReplayTimelinePlanner::build(blocks, 100, 120, ReplayMode::AutoHigh);
    ASSERT_EQ(1u, clips.size());
    EXPECT_EQ("first.mp4", clips[0].file_path);

    ReplayPlanOptions options;
    options.max_gap_s = 3;
    EXPECT_EQ(2u, ReplayTimelinePlanner::build(blocks, 100, 120, ReplayMode::AutoHigh, options).size());
}

TEST(ReplayTimelinePlannerTest, ShortPreferredStretchesDoNotFlapBetweenStreams) {
    // Sub covers the whole range; Main has short holes every few seconds.
    std::vector<ReplayBlock> blocks;
    blocks.push_back(block(1, 100, 160, "sub.mp4"));
    blocks.push_back(block(0, 100, 110, "main1.mp4"));
    blocks.push_back(block(0, 112, 115, "main2.mp4"));
    blocks.push_back(block(0, 117, 120, "main3.mp4"));
    blocks.push_back(block(0, 122, 160, "main4.mp4"));

    const auto clips = ReplayTimelinePlanner::build(blocks, 100, 160, ReplayMode::AutoHigh);
    ASSERT_EQ(3u, clips.size());
    EXPECT_EQ("main1.mp4", clips[0].file_path);
    EXPECT_EQ(10u, clips[0].duration);
    EXPECT_EQ("sub.mp4", clips[1].file_path);
    EXPECT_EQ(110u, clips[1].start_time);
    EXPECT_EQ(12u, clips[1].duration);
    EXPECT_EQ("main4.mp4", clips[2].file_path);
    EXPECT_EQ(122u, clips[2].start_time);

    ReplayPlanOptions no_hysteresis;
    no_hysteresis.min_switch_s = 0;
    EXPECT_EQ(7u, ReplayTimelinePlanner::build(blocks, 100, 160, ReplayMode::AutoHigh, no_hysteresis).size());
}
