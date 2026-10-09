#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>
#include <unistd.h>

#include "Common/BlockingProcessRunner.h"
#include "Common/config.h"
#include "Local/TierMediaTransformer.h"
#include "Util/File.h"
#include "Util/mini.h"
#if defined(ENABLE_FFMPEG)
#include "Codec/HwCodec.h"
#endif

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

namespace {

class TierMediaTransformerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        char root[] = "/tmp/s3mk-tier-transcode-XXXXXX";
        ASSERT_NE(nullptr, mkdtemp(root));
        _root = root;
        _source = _root + "/source.mp4";
        mINI::Instance()[Storage::kTierTranscodeFfmpegBin] = "ffmpeg";
        mINI::Instance()[Storage::kTierTranscodeFfprobeBin] = "ffprobe";
        mINI::Instance()[Storage::kTierTranscodeStagingPath] = _root + "/staging";
        mINI::Instance()[Storage::kTierTranscodeTimeoutSeconds] = 60;
    }

    void TearDown() override { File::delete_file(_root, true); }

    BlockingProcessResult generateSource(const std::string &encoder = "mpeg4") {
        BlockingProcessRequest request;
        request.executable = "ffmpeg";
        request.argv = {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                        "-i", "testsrc=size=640x360:rate=24", "-t", "1", "-an",
                        "-c:v", encoder, "-b:v", "4M", "-pix_fmt", "yuv420p", _source};
        request.timeout_seconds = 30;
        return BlockingProcessRunner().run(request);
    }

    // Returns the encoder that produced the output ("" when skipped or failed).
    std::string runCodecCase(const std::string &codec, const std::string &encoder,
                             const std::string &source_encoder = "mpeg4") {
        BlockingProcessResult generated = generateSource(source_encoder);
        if (!generated.success())
            return skip("ffmpeg unavailable: " + generated.standard_error);
        if (codec == "H264")
            mINI::Instance()[Storage::kTierTranscodeH264Encoders] = encoder;
        else
            mINI::Instance()[Storage::kTierTranscodeH265Encoders] = encoder;

        FfmpegTierMediaTransformer transformer;
        TierTranscodeCapabilities capabilities = transformer.probe();
        if ((codec == "H264" && !capabilities.h264_available) ||
            (codec == "H265" && !capabilities.h265_available))
            return skip(encoder + " unavailable");

        TierTransformRequest request;
        request.job_id = "integration-job";
        request.stream_id = "main";
        request.segment_path = "2026/01/01/source.mp4";
        request.source_path = _source;
        request.profile.enabled = true;
        request.profile.video_codec = codec;
        request.profile.max_width = 320;
        request.profile.max_height = 180;
        request.profile.max_fps = 12;
        request.profile.max_video_bitrate_kbps = 300;
        TierTransformResult result = transformer.transform(request);
        EXPECT_TRUE(result.success) << result.error_code << ": " << result.error_message;
        if (!result.success) return std::string();
        EXPECT_GT(result.output_bytes, 0);

        TierMediaDescriptor output;
        std::string error;
        EXPECT_TRUE(transformer.inspect(result.output_path, output, error)) << error;
        EXPECT_EQ(codec, output.video_codec);
        EXPECT_LE(output.width, 320);
        EXPECT_LE(output.height, 180);
        EXPECT_LE(output.fps, 12.1);
        return result.encoder_backend;
    }

    std::string skip(const std::string &reason) {
        _skipped = reason;
        return std::string();
    }

    std::string _skipped;

    std::string _root;
    std::string _source;
};

TEST_F(TierMediaTransformerIntegrationTest, ProducesCappedH264) {
    runCodecCase("H264", "libx264");
    if (!_skipped.empty()) GTEST_SKIP() << _skipped;
}

TEST_F(TierMediaTransformerIntegrationTest, ProducesCappedH265) {
    runCodecCase("H265", "libx265");
    if (!_skipped.empty()) GTEST_SKIP() << _skipped;
}

#if defined(ENABLE_FFMPEG)
// The real GPU of hwcodec, skipped without one. WSL dev box: modprobe vgem, LIBVA_DRIVER_NAME=d3d12
// (H.264 only, hevc_vaapi crashes in that driver and the probe turns it off).
TEST_F(TierMediaTransformerIntegrationTest, EncodesH264OnTheProbedGpu) {
    HwCodecManager::Instance().probe();
    if (HwCodecManager::Instance().backends().empty()) GTEST_SKIP() << "no GPU backend";
    mINI::Instance()[Storage::kTierTranscodeHwaccel] = 1;
    const uint64_t fallbacks = HwCodecManager::Instance().fallbackCount();
    const std::string encoder = runCodecCase("H264", "libx264", "libx264");
    if (!_skipped.empty()) GTEST_SKIP() << _skipped;
    EXPECT_NE("libx264", encoder) << "GPU " << HwCodecManager::Instance().backends()
                                  << " fell back to the CPU";
    EXPECT_EQ(fallbacks, HwCodecManager::Instance().fallbackCount());
    EXPECT_EQ(0U, HwCodecManager::Instance().hwEncoderCount());
    EXPECT_EQ(0U, HwCodecManager::Instance().hwDecoderCount());
    std::cout << "backends=" << HwCodecManager::Instance().backends() << " encoder=" << encoder
              << std::endl;
}
#endif

} // namespace
