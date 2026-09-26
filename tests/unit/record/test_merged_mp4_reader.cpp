#include <gtest/gtest.h>

#ifdef ENABLE_MP4
#include <cstdlib>

#include "Record/MergedMP4Reader.h"

using namespace mediakit;

TEST(MergedMP4ReaderTest, ParsesReplayStartTime) {
    uint64_t start = 0;
    EXPECT_TRUE(parseReplayStartTime("cam1/record/1758790000.replay.0123abcd", start));
    EXPECT_EQ(1758790000u, start);
    EXPECT_TRUE(parseReplayStartTime("cam1/record/1758790001", start));
    EXPECT_EQ(1758790001u, start);

    EXPECT_FALSE(parseReplayStartTime("cam1/1758790000.replay.0123abcd", start));
    EXPECT_FALSE(parseReplayStartTime("cam1/record/17587x0000.replay.0123abcd", start));
    EXPECT_FALSE(parseReplayStartTime("cam1/record/.replay.0123abcd", start));
    EXPECT_FALSE(parseReplayStartTime("cam1/record/-5", start));
}

namespace {

Broadcast::ReplayClip makeClip(int source, uint64_t start, uint32_t duration, uint64_t file_start) {
    Broadcast::ReplayClip clip;
    clip.source = source;
    clip.start_time = start;
    clip.duration = duration;
    clip.file_start_time = file_start;
    return clip;
}

// MP4Demuxer::seekTo semantics over a keyframe list: nearest keyframe wins.
int64_t nearestKeyframe(const std::vector<int64_t> &keyframes, int64_t stamp) {
    int64_t best = keyframes.front();
    for (auto keyframe : keyframes) {
        if (std::llabs(keyframe - stamp) < std::llabs(best - stamp)) best = keyframe;
    }
    return best;
}

// Feed frames of a clip every 40 ms from raw_begin, the way readTimelineTick
// filters them; returns the emitted timeline DTS values.
std::vector<int64_t> playClip(const ReplayClipWindow &window, int64_t raw_begin) {
    std::vector<int64_t> emitted;
    for (int64_t raw = raw_begin; raw < raw_begin + 60000; raw += 40) {
        const auto mapped = window.map(raw);
        if (window.pastEnd(mapped)) break;
        if (!window.beforeStart(mapped)) emitted.push_back(mapped);
    }
    return emitted;
}

} // namespace

TEST(MergedMP4ReaderTest, BuildsClipWindowFromReplayClip) {
    const auto window = makeReplayClipWindow(makeClip(1, 1004, 4, 990), 1000);
    EXPECT_EQ(4000, window.start_ms);
    EXPECT_EQ(8000, window.end_ms);
    EXPECT_EQ(14000, window.file_offset_ms);
    EXPECT_EQ(4000, window.play_from_ms);
    EXPECT_EQ(4000, window.map(14000));
}

TEST(MergedMP4ReaderTest, SeeksToKeyframeAtOrBeforeTarget) {
    const std::vector<int64_t> keyframes = {0, 2000, 4000, 6000};
    auto seek = [&keyframes](int64_t stamp) { return nearestKeyframe(keyframes, stamp); };

    EXPECT_EQ(4000, seekKeyframeAtOrBefore(seek, 4000));
    EXPECT_EQ(4000, seekKeyframeAtOrBefore(seek, 4500));
    // Nearest keyframe to 5900 is 6000: step back to 4000.
    EXPECT_EQ(4000, seekKeyframeAtOrBefore(seek, 5900));
    EXPECT_EQ(-1, seekKeyframeAtOrBefore([](int64_t) { return int64_t(-1); }, 5000));

    const std::vector<int64_t> late_first = {3000, 6000};
    EXPECT_EQ(3000, seekKeyframeAtOrBefore([&late_first](int64_t stamp) {
        return nearestKeyframe(late_first, stamp);
    }, 1000));
}

TEST(MergedMP4ReaderTest, SwitchPointMustLieInsideThePreviousClip) {
    auto prev = makeReplayClipWindow(makeClip(0, 1000, 4, 1000), 1000);  // [0, 4000)
    auto next = makeReplayClipWindow(makeClip(1, 1004, 4, 990), 1000);   // [4000, 8000), offset 14000
    int64_t switch_ms = 0;

    ASSERT_TRUE(replaySwitchPoint(prev, next, 12500, switch_ms));
    EXPECT_EQ(2500, switch_ms);
    ASSERT_TRUE(replaySwitchPoint(prev, next, 14000, switch_ms));
    EXPECT_EQ(4000, switch_ms);

    EXPECT_FALSE(replaySwitchPoint(prev, next, -1, switch_ms));     // seek failed
    EXPECT_FALSE(replaySwitchPoint(prev, next, 14500, switch_ms));  // keyframe after boundary
    EXPECT_FALSE(replaySwitchPoint(prev, next, 9000, switch_ms));   // before previous clip
    next.start_ms = 5000;
    EXPECT_FALSE(replaySwitchPoint(prev, next, 12500, switch_ms));  // not adjacent
}

// Main clip [0, 4s), then Sub clip [4s, 8s) whose file started 10 s earlier and
// has its keyframe 1.5 s before the boundary. Without a switch point the Sub
// frames before 4 s are dropped and playback waits for the next Sub IDR; with
// the switch point Main ends at the Sub keyframe and Sub plays from it.
TEST(MergedMP4ReaderTest, MainToSubSwitchPlaysFromNextKeyframeWithoutGap) {
    auto main_clip = makeReplayClipWindow(makeClip(0, 1000, 4, 1000), 1000);
    auto sub_clip = makeReplayClipWindow(makeClip(1, 1004, 4, 990), 1000);
    const std::vector<int64_t> sub_keyframes = {0, 3000, 6000, 9000, 12500, 16000};
    const int64_t landed = seekKeyframeAtOrBefore([&sub_keyframes](int64_t stamp) {
        return nearestKeyframe(sub_keyframes, stamp);
    }, sub_clip.file_offset_ms);
    ASSERT_EQ(12500, landed);

    int64_t switch_ms = 0;
    ASSERT_TRUE(replaySwitchPoint(main_clip, sub_clip, landed, switch_ms));
    main_clip.end_ms = switch_ms;
    sub_clip.play_from_ms = switch_ms;

    auto emitted = playClip(main_clip, 0);
    const auto sub_frames = playClip(sub_clip, landed);
    ASSERT_FALSE(sub_frames.empty());
    EXPECT_EQ(switch_ms, sub_frames.front());  // first Sub frame is its keyframe
    emitted.insert(emitted.end(), sub_frames.begin(), sub_frames.end());
    for (size_t i = 1; i < emitted.size(); ++i) {
        ASSERT_LT(emitted[i - 1], emitted[i]) << "at " << i;
        ASSERT_LE(emitted[i] - emitted[i - 1], 40) << "gap at " << i;
    }
    EXPECT_LT(emitted.back(), 8000);
}

// Main clip [0, 4s) from a file starting at the replay start, then a Sub clip
// [4s, 8s) whose file started 10 s earlier. The Sub seek lands on a keyframe
// 1.5 s before the clip start; merged DTS must still increase monotonically.
TEST(MergedMP4ReaderTest, DropsFramesBeforeClipStartAfterKeyframeSeek) {
    const auto main_clip = makeReplayClipWindow(makeClip(0, 1000, 4, 1000), 1000);
    const auto sub_clip = makeReplayClipWindow(makeClip(1, 1004, 4, 990), 1000);

    std::vector<int64_t> emitted;
    for (int64_t raw = 0; raw < 5000; raw += 40) {
        const auto mapped = main_clip.map(raw);
        if (main_clip.pastEnd(mapped)) break;
        if (!main_clip.beforeStart(mapped)) emitted.push_back(mapped);
    }
    ASSERT_FALSE(emitted.empty());
    EXPECT_LT(emitted.back(), 4000);

    size_t dropped = 0;
    for (int64_t raw = 12500; raw < 20000; raw += 40) {
        const auto mapped = sub_clip.map(raw);
        if (sub_clip.pastEnd(mapped)) break;
        if (sub_clip.beforeStart(mapped)) {
            ++dropped;
            continue;
        }
        emitted.push_back(mapped);
    }
    EXPECT_GT(dropped, 0u);
    for (size_t i = 1; i < emitted.size(); ++i) {
        ASSERT_LT(emitted[i - 1], emitted[i]) << "at " << i;
    }
    EXPECT_EQ(4000, sub_clip.map(14000));
    EXPECT_LT(emitted.back(), 8000);
}
#endif
