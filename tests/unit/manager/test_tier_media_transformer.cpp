#include <gtest/gtest.h>

#include "Local/TierMediaTransformer.h"

using namespace managerkit;

TEST(TierMediaTransformerTest, StagingJobKeyIsStableAndOpaque) {
    const std::string first = tierTranscodeStagingJobKey("job/with unsafe chars");
    EXPECT_EQ(24U, first.size());
    EXPECT_EQ(first, tierTranscodeStagingJobKey("job/with unsafe chars"));
    EXPECT_NE(first, tierTranscodeStagingJobKey("another-job"));
    EXPECT_EQ(std::string::npos, first.find('/'));
}

TEST(TierMediaTransformerTest, PlansAspectPreservingEvenDownscale) {
    FfmpegTierMediaTransformer transformer;
    TierMediaDescriptor source;
    source.video_codec = "H264";
    source.width = 1920;
    source.height = 1080;
    source.fps = 30;
    source.video_bitrate_kbps = 4000;
    TierQualityProfile profile;
    profile.enabled = true;
    profile.video_codec = "H264";
    profile.max_width = 1280;
    profile.max_height = 720;
    profile.max_fps = 15;
    profile.max_video_bitrate_kbps = 1200;

    TierTransformDecision decision = transformer.plan(source, profile);
    EXPECT_EQ(TierTransformAction::TRANSCODE, decision.action);
    EXPECT_EQ(1280, decision.target_width);
    EXPECT_EQ(720, decision.target_height);
    EXPECT_EQ(15, decision.target_fps);
    EXPECT_EQ(1200, decision.target_video_bitrate_kbps);
}

TEST(TierMediaTransformerTest, DoesNotUpscaleOrIncreaseFpsOrBitrate) {
    FfmpegTierMediaTransformer transformer;
    TierMediaDescriptor source;
    source.video_codec = "H264";
    source.width = 640;
    source.height = 360;
    source.fps = 12;
    source.video_bitrate_kbps = 500;
    TierQualityProfile profile;
    profile.enabled = true;
    profile.video_codec = "H264";
    profile.max_width = 1280;
    profile.max_height = 720;
    profile.max_fps = 15;
    profile.max_video_bitrate_kbps = 1200;

    TierTransformDecision decision = transformer.plan(source, profile);
    EXPECT_EQ(TierTransformAction::COPY_POLICY_NOOP, decision.action);
    EXPECT_EQ(640, decision.target_width);
    EXPECT_EQ(360, decision.target_height);
    EXPECT_EQ(0, decision.target_fps);
}

TEST(TierMediaTransformerTest, CodecChangeRequiresTranscode) {
    FfmpegTierMediaTransformer transformer;
    TierMediaDescriptor source;
    source.video_codec = "H264";
    source.width = 640;
    source.height = 360;
    source.fps = 10;
    source.video_bitrate_kbps = 400;
    TierQualityProfile profile;
    profile.enabled = true;
    profile.video_codec = "H265";
    profile.max_video_bitrate_kbps = 800;
    EXPECT_EQ(TierTransformAction::TRANSCODE, transformer.plan(source, profile).action);
}
