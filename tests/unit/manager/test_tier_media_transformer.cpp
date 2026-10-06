#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <unistd.h>

#include "Common/config.h"
#include "Local/TierMediaTransformer.h"
#include "Util/File.h"
#include "Util/mini.h"

using namespace managerkit;
using namespace toolkit;

namespace {

// Answers ffprobe with canned JSON (source vs. staged output) and records ffmpeg argv,
// writing an output file of the configured size so validate() sees it.
class FakeProcessRunner : public IBlockingProcessRunner {
public:
    BlockingProcessResult run(const BlockingProcessRequest &request) override {
        BlockingProcessResult result;
        result.exit_code = 0;
        if (request.executable == "ffprobe") {
            const bool output = request.argv.back() != source_path;
            result.standard_output = probeJson(output ? output_probe : source_probe);
            return result;
        }
        ffmpeg_argv = request.argv;
        std::ofstream(request.argv.back().c_str()) << std::string(output_size, 'x');
        return result;
    }

    bool hasArgs(const std::string &key, const std::string &value) const {
        for (size_t i = 0; i + 1 < ffmpeg_argv.size(); ++i) {
            if (ffmpeg_argv[i] == key && ffmpeg_argv[i + 1] == value) return true;
        }
        return false;
    }

    struct Probe {
        std::string video_codec = "h264";
        int width = 640;
        int height = 360;
        std::string fps = "25/1";
        std::string bit_rate = "600000";
        std::string audio_codec;
    };

    std::string source_path;
    Probe source_probe;
    Probe output_probe;
    size_t output_size = 100;
    std::vector<std::string> ffmpeg_argv;

private:
    static std::string probeJson(const Probe &probe) {
        std::string json = "{\"streams\":[{\"codec_type\":\"video\",\"codec_name\":\"" +
            probe.video_codec + "\",\"width\":" + std::to_string(probe.width) +
            ",\"height\":" + std::to_string(probe.height) + ",\"avg_frame_rate\":\"" +
            probe.fps + "\",\"bit_rate\":\"" + probe.bit_rate + "\"}";
        if (!probe.audio_codec.empty())
            json += ",{\"codec_type\":\"audio\",\"codec_name\":\"" + probe.audio_codec + "\"}";
        return json + "],\"format\":{\"duration\":\"20\"}}";
    }
};

class TierMediaTransformerFakeRunnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        char root[] = "/tmp/s3mk-tier-transform-unit-XXXXXX";
        ASSERT_NE(nullptr, mkdtemp(root));
        _root = root;
        _runner = std::make_shared<FakeProcessRunner>();
        _runner->source_path = _root + "/source.mp4";
        std::ofstream(_runner->source_path.c_str()) << std::string(1000, 's');
        mINI::Instance()[mediakit::Storage::kTierTranscodeFfmpegBin] = "ffmpeg";
        mINI::Instance()[mediakit::Storage::kTierTranscodeFfprobeBin] = "ffprobe";
        mINI::Instance()[mediakit::Storage::kTierTranscodeStagingPath] = _root + "/staging";
        mINI::Instance()[mediakit::Storage::kTierTranscodeH264Encoders] = "libx264";
        mINI::Instance()[mediakit::Storage::kTierTranscodeH265Encoders] = "libx265";
    }

    void TearDown() override { File::delete_file(_root, true); }

    TierTransformResult transform(const std::string &codec = "H264") {
        FfmpegTierMediaTransformer transformer(_runner);
        TierTransformRequest request;
        request.job_id = "unit-job";
        request.stream_id = "sub";
        request.segment_path = "2026/10/06/source.mp4";
        request.source_path = _runner->source_path;
        request.profile.enabled = true;
        request.profile.video_codec = codec;
        request.profile.max_fps = 10;
        request.profile.max_video_bitrate_kbps = 800;
        return transformer.transform(request);
    }

    std::string _root;
    std::shared_ptr<FakeProcessRunner> _runner;
};

} // namespace

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
    EXPECT_EQ(1200, decision.max_video_bitrate_kbps);
}

TEST(TierMediaTransformerTest, ForcedTranscodeKeepsLowerSourceBitrate) {
    FfmpegTierMediaTransformer transformer;
    TierMediaDescriptor source;
    source.video_codec = "H264";
    source.width = 640;
    source.height = 360;
    source.fps = 25;
    source.video_bitrate_kbps = 600;
    TierQualityProfile profile;
    profile.enabled = true;
    profile.video_codec = "H264";
    profile.max_fps = 10;
    profile.max_video_bitrate_kbps = 800;

    TierTransformDecision decision = transformer.plan(source, profile);
    EXPECT_EQ(TierTransformAction::TRANSCODE, decision.action);
    EXPECT_EQ(600, decision.target_video_bitrate_kbps);
    EXPECT_EQ(800, decision.max_video_bitrate_kbps);
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

TEST_F(TierMediaTransformerFakeRunnerTest, DisablesBFramesAndSetsTwoSecondGop) {
    _runner->output_probe.fps = "10/1";
    TierTransformResult result = transform();
    ASSERT_TRUE(result.success) << result.error_code << ": " << result.error_message;
    EXPECT_FALSE(result.copied_without_transcode);
    EXPECT_TRUE(_runner->hasArgs("-bf", "0"));
    EXPECT_TRUE(_runner->hasArgs("-g", "20"));
    EXPECT_TRUE(_runner->hasArgs("-b:v", "600k"));
    EXPECT_TRUE(_runner->hasArgs("-f", "mp4"));
}

TEST_F(TierMediaTransformerFakeRunnerTest, KeepsSourceWhenSameCodecOutputIsLarger) {
    _runner->output_probe.fps = "10/1";
    _runner->output_size = 1000;
    TierTransformResult result = transform();
    ASSERT_TRUE(result.success) << result.error_code << ": " << result.error_message;
    EXPECT_TRUE(result.copied_without_transcode);
    EXPECT_EQ(_runner->source_path, result.output_path);
    EXPECT_EQ(1000, result.output_bytes);
    EXPECT_FALSE(File::fileExist(_runner->ffmpeg_argv.back()));
}

TEST_F(TierMediaTransformerFakeRunnerTest, AcceptsLargerOutputWhenCodecChanges) {
    _runner->output_probe.video_codec = "hevc";
    _runner->output_probe.fps = "10/1";
    _runner->output_size = 1000;
    TierTransformResult result = transform("H265");
    ASSERT_TRUE(result.success) << result.error_code << ": " << result.error_message;
    EXPECT_FALSE(result.copied_without_transcode);
    EXPECT_EQ(_runner->ffmpeg_argv.back(), result.output_path);
}

TEST_F(TierMediaTransformerFakeRunnerTest, ReencodesG711ToAac) {
    _runner->source_probe.audio_codec = "pcm_alaw";
    _runner->output_probe.fps = "10/1";
    _runner->output_probe.audio_codec = "aac";
    ASSERT_TRUE(transform().success);
    EXPECT_TRUE(_runner->hasArgs("-c:a", "aac"));
    EXPECT_TRUE(_runner->hasArgs("-b:a", "32k"));
    EXPECT_TRUE(_runner->hasArgs("-f", "mp4"));
}

TEST_F(TierMediaTransformerFakeRunnerTest, CopiesAacIntoMp4) {
    _runner->source_probe.audio_codec = "aac";
    _runner->output_probe.fps = "10/1";
    ASSERT_TRUE(transform().success);
    EXPECT_TRUE(_runner->hasArgs("-c:a", "copy"));
    EXPECT_TRUE(_runner->hasArgs("-f", "mp4"));
}
