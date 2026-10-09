#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <unistd.h>

#include "Common/config.h"
#include "Local/TierMediaTransformer.h"
#if defined(ENABLE_FFMPEG)
#include "Codec/HwCodec.h"
#endif
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
        ffmpeg_runs.push_back(request.argv);
        if (fail_ffmpeg && fail_ffmpeg(request.argv)) {
            result.exit_code = 1;
            return result;
        }
        if (request.argv.back() == "-") return result; // encoder smoke test of probe()
        ffmpeg_argv = request.argv;
        std::ofstream(request.argv.back().c_str()) << std::string(output_size, 'x');
        return result;
    }

    bool hasArgs(const std::string &key, const std::string &value) const {
        return hasArgs(ffmpeg_argv, key, value);
    }

    static bool hasArgs(const std::vector<std::string> &argv, const std::string &key,
                        const std::string &value) {
        for (size_t i = 0; i + 1 < argv.size(); ++i) {
            if (argv[i] == key && argv[i + 1] == value) return true;
        }
        return false;
    }

    static std::string argAfter(const std::vector<std::string> &argv, const std::string &key) {
        for (size_t i = 0; i + 1 < argv.size(); ++i) {
            if (argv[i] == key) return argv[i + 1];
        }
        return std::string();
    }

    static size_t indexOf(const std::vector<std::string> &argv, const std::string &value) {
        return std::find(argv.begin(), argv.end(), value) - argv.begin();
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
    std::vector<std::vector<std::string>> ffmpeg_runs;
    std::function<bool(const std::vector<std::string> &)> fail_ffmpeg;

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
        return transform(transformer, codec);
    }

    TierTransformResult transform(FfmpegTierMediaTransformer &transformer,
                                  const std::string &codec = "H264") {
        TierTransformRequest request;
        request.job_id = "unit-job";
        request.stream_id = "sub";
        request.segment_path = "2026/10/06/source.mp4";
        request.source_path = _runner->source_path;
        request.profile.enabled = true;
        request.profile.video_codec = codec;
        request.profile.max_fps = 10;
        request.profile.max_video_bitrate_kbps = 800;
        request.profile.max_width = _max_width;
        request.profile.max_height = _max_height;
        return transformer.transform(request);
    }

    std::string _root;
    std::shared_ptr<FakeProcessRunner> _runner;
    int _max_width = 0;
    int _max_height = 0;
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

TEST_F(TierMediaTransformerFakeRunnerTest, TriesOnlyEncodersThatPassedTheProbe) {
    mINI::Instance()[mediakit::Storage::kTierTranscodeH264Encoders] = "h264_nvenc,libx264";
    mINI::Instance()[mediakit::Storage::kTierTranscodeHwaccel] = 0;
    _runner->fail_ffmpeg = [](const std::vector<std::string> &argv) {
        return std::find(argv.begin(), argv.end(), "h264_nvenc") != argv.end();
    };
    _runner->output_probe.fps = "10/1";
    FfmpegTierMediaTransformer transformer(_runner);
    TierTranscodeCapabilities capabilities = transformer.probe();
    EXPECT_TRUE(capabilities.probed);
    EXPECT_EQ(std::vector<std::string>{"libx264"}, capabilities.h264_encoders);
    _runner->ffmpeg_runs.clear();

    TierTransformResult result = transform(transformer);
    ASSERT_TRUE(result.success) << result.error_code << ": " << result.error_message;
    ASSERT_EQ(1U, _runner->ffmpeg_runs.size());
    EXPECT_EQ("libx264", FakeProcessRunner::argAfter(_runner->ffmpeg_runs[0], "-c:v"));
    EXPECT_EQ("libx264", result.encoder_backend);
    mINI::Instance()[mediakit::Storage::kTierTranscodeHwaccel] = 1;
}

#if defined(ENABLE_FFMPEG)
namespace {

using mediakit::HwBackend;
using mediakit::HwBackendCaps;
using mediakit::HwCodecManager;
using namespace mediakit;

class ConfigGuard {
public:
    ConfigGuard(const std::string &key, const std::string &value) : _key(key) {
        auto &ini = mINI::Instance();
        auto it = ini.find(key);
        _old = it == ini.end() ? "" : it->second;
        ini[key] = value;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    }
    ~ConfigGuard() {
        mINI::Instance()[_key] = _old;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    }

private:
    std::string _key;
    std::string _old;
};

// Backends that probe as working for H.264/H.265: decode when `decode`, always encode.
void probeBackends(const std::vector<HwBackend> &backends, bool decode) {
    HwCodecManager::Instance().probe(
        [backends, decode](HwBackend backend, const std::string &, HwBackendCaps &caps,
                           AVBufferRef *&) {
            if (std::find(backends.begin(), backends.end(), backend) == backends.end())
                return false;
            caps.decode[0] = caps.decode[1] = decode;
            caps.encode[0] = caps.encode[1] = true;
            return true;
        });
}

bool usesGpuDecode(const std::vector<std::string> &argv) {
    return std::find(argv.begin(), argv.end(), "-hwaccel") != argv.end();
}

class TierTranscodeGpuTest : public TierMediaTransformerFakeRunnerTest {
protected:
    void SetUp() override {
        TierMediaTransformerFakeRunnerTest::SetUp();
        _runner->output_probe.fps = "10/1";
    }
    void TearDown() override {
        probeBackends({}, false);
        TierMediaTransformerFakeRunnerTest::TearDown();
    }

    ConfigGuard _enable { mediakit::HwCodec::kEnable, "1" };
    ConfigGuard _decode { mediakit::HwCodec::kDecode, "1" };
    ConfigGuard _encode { mediakit::HwCodec::kEncode, "1" };
    ConfigGuard _nvidia { mediakit::General::kCheckNvidiaDev, "1" };
    ConfigGuard _backends { mediakit::HwCodec::kBackends, "cuda,vaapi,qsv" };
    ConfigGuard _max_dec { mediakit::HwCodec::kMaxDecodeSessions, "0" };
    ConfigGuard _max_enc { mediakit::HwCodec::kMaxEncodeSessions, "0" };
    ConfigGuard _device { mediakit::HwCodec::kDevice, "" };
    ConfigGuard _dec_budget { mediakit::HwCodec::kDecodeBudget, "" };
    ConfigGuard _enc_budget { mediakit::HwCodec::kEncodeBudget, "" };
    // A failed GPU run would otherwise keep the backend off for the next tests.
    ConfigGuard _cooldown { mediakit::HwCodec::kCooldownSec, "0" };
    ConfigGuard _hwaccel { mediakit::Storage::kTierTranscodeHwaccel, "1" };
};

} // namespace

TEST_F(TierTranscodeGpuTest, DecodesScalesAndEncodesOnCuda) {
    _max_width = 320;
    _max_height = 180;
    _runner->output_probe.width = 320;
    _runner->output_probe.height = 180;
    probeBackends({HwBackend::Cuda}, true);
    TierTransformResult result = transform();
    ASSERT_TRUE(result.success) << result.error_code << ": " << result.error_message;
    ASSERT_EQ(1U, _runner->ffmpeg_runs.size());
    const std::vector<std::string> &argv = _runner->ffmpeg_argv;
    EXPECT_TRUE(FakeProcessRunner::hasArgs(argv, "-hwaccel", "cuda"));
    EXPECT_TRUE(FakeProcessRunner::hasArgs(argv, "-hwaccel_output_format", "cuda"));
    EXPECT_LT(FakeProcessRunner::indexOf(argv, "-hwaccel"), FakeProcessRunner::indexOf(argv, "-i"));
    EXPECT_EQ("fps=10,scale_cuda=320:180", FakeProcessRunner::argAfter(argv, "-vf"));
    EXPECT_EQ("h264_nvenc", FakeProcessRunner::argAfter(argv, "-c:v"));
    EXPECT_TRUE(FakeProcessRunner::hasArgs(argv, "-maxrate", "600k"));
    EXPECT_TRUE(FakeProcessRunner::hasArgs(argv, "-bf", "0"));
    EXPECT_EQ(argv.size(), FakeProcessRunner::indexOf(argv, "-pix_fmt"));
    EXPECT_EQ("h264_nvenc", result.encoder_backend);
    EXPECT_EQ(0U, HwCodecManager::Instance().hwEncoderCount());
    EXPECT_EQ(0U, HwCodecManager::Instance().hwDecoderCount());
}

TEST_F(TierTranscodeGpuTest, FailedGpuDecodeRetriesWithGpuEncodeOnly) {
    probeBackends({HwBackend::Cuda}, true);
    _runner->fail_ffmpeg = usesGpuDecode;
    TierTransformResult result = transform();
    ASSERT_TRUE(result.success) << result.error_code << ": " << result.error_message;
    ASSERT_EQ(2U, _runner->ffmpeg_runs.size());
    EXPECT_TRUE(usesGpuDecode(_runner->ffmpeg_runs[0]));
    EXPECT_FALSE(usesGpuDecode(_runner->ffmpeg_runs[1]));
    EXPECT_EQ("h264_nvenc", FakeProcessRunner::argAfter(_runner->ffmpeg_runs[1], "-c:v"));
    EXPECT_TRUE(FakeProcessRunner::hasArgs(_runner->ffmpeg_runs[1], "-pix_fmt", "yuv420p"));
    EXPECT_EQ("fps=10", FakeProcessRunner::argAfter(_runner->ffmpeg_runs[1], "-vf"));
}

TEST_F(TierTranscodeGpuTest, FailedGpuEncoderFallsBackToCpuAndCoolsDown) {
    ConfigGuard cooldown(mediakit::HwCodec::kCooldownSec, "300");
    // qsv: no other test uses it, so its cooldown cannot leak.
    probeBackends({HwBackend::Qsv}, false);
    _runner->fail_ffmpeg = [](const std::vector<std::string> &argv) {
        return FakeProcessRunner::argAfter(argv, "-c:v") == "h264_qsv";
    };
    const uint64_t fallbacks = HwCodecManager::Instance().fallbackCount();
    TierTransformResult result = transform();
    ASSERT_TRUE(result.success) << result.error_code << ": " << result.error_message;
    ASSERT_EQ(2U, _runner->ffmpeg_runs.size());
    EXPECT_TRUE(FakeProcessRunner::hasArgs(_runner->ffmpeg_runs[0], "-pix_fmt", "nv12"));
    EXPECT_EQ("libx264", FakeProcessRunner::argAfter(_runner->ffmpeg_runs[1], "-c:v"));
    EXPECT_EQ("libx264", result.encoder_backend);
    EXPECT_EQ(fallbacks + 1, HwCodecManager::Instance().fallbackCount());
    EXPECT_EQ(0U, HwCodecManager::Instance().hwEncoderCount());

    // The next segment leaves the backend alone during the cooldown.
    _runner->ffmpeg_runs.clear();
    ASSERT_TRUE(transform().success);
    ASSERT_EQ(1U, _runner->ffmpeg_runs.size());
    EXPECT_EQ("libx264", FakeProcessRunner::argAfter(_runner->ffmpeg_runs[0], "-c:v"));
}

TEST_F(TierTranscodeGpuTest, VaapiEncodeUploadsCpuFrames) {
    probeBackends({HwBackend::Vaapi}, false);
    TierTransformResult result = transform();
    ASSERT_TRUE(result.success) << result.error_code << ": " << result.error_message;
    const std::vector<std::string> &argv = _runner->ffmpeg_argv;
    EXPECT_TRUE(FakeProcessRunner::hasArgs(argv, "-init_hw_device", "vaapi=gpu"));
    EXPECT_LT(FakeProcessRunner::indexOf(argv, "-init_hw_device"),
              FakeProcessRunner::indexOf(argv, "-i"));
    EXPECT_EQ("fps=10,format=nv12,hwupload", FakeProcessRunner::argAfter(argv, "-vf"));
    EXPECT_EQ("h264_vaapi", FakeProcessRunner::argAfter(argv, "-c:v"));
    EXPECT_TRUE(FakeProcessRunner::hasArgs(argv, "-rc_mode", "VBR"));
}

TEST_F(TierTranscodeGpuTest, VaapiDecodeAlwaysScalesToNv12) {
    probeBackends({HwBackend::Vaapi}, true);
    ASSERT_TRUE(transform().success);
    const std::vector<std::string> &argv = _runner->ffmpeg_argv;
    EXPECT_TRUE(FakeProcessRunner::hasArgs(argv, "-hwaccel", "vaapi"));
    EXPECT_EQ("fps=10,scale_vaapi=format=nv12", FakeProcessRunner::argAfter(argv, "-vf"));
    EXPECT_EQ(argv.size(), FakeProcessRunner::indexOf(argv, "-init_hw_device"));
}

TEST_F(TierTranscodeGpuTest, MjpegSourceIsNotDecodedOnTheGpu) {
    _runner->source_probe.video_codec = "mjpeg";
    probeBackends({HwBackend::Cuda}, true);
    ASSERT_TRUE(transform().success);
    EXPECT_FALSE(usesGpuDecode(_runner->ffmpeg_argv));
    EXPECT_EQ("h264_nvenc", FakeProcessRunner::argAfter(_runner->ffmpeg_argv, "-c:v"));
}

TEST_F(TierTranscodeGpuTest, PicksTheLessLoadedCard) {
    ConfigGuard device(mediakit::HwCodec::kDevice, "cuda:0;1");
    probeBackends({HwBackend::Cuda}, false);
    auto live = HwCodecManager::Instance().acquireEncode(mediakit::CodecH264, 1920, 1080, 30);
    ASSERT_TRUE(live);
    ASSERT_EQ("0", live->deviceName());
    ASSERT_TRUE(transform().success);
    EXPECT_TRUE(_runner->hasArgs("-gpu", "1"));
}

TEST_F(TierTranscodeGpuTest, DecodeOnAnotherCardEncodesOnly) {
    ConfigGuard device(mediakit::HwCodec::kDevice, "cuda:0;1");
    probeBackends({HwBackend::Cuda}, true);
    // Encode lighter on card 1, decode lighter on card 0: the two sessions land on different cards.
    auto live_encode = HwCodecManager::Instance().acquireEncode(mediakit::CodecH264);
    auto live_decode0 = HwCodecManager::Instance().acquireDecode(mediakit::CodecH264);
    auto live_decode1 = HwCodecManager::Instance().acquireDecode(mediakit::CodecH264);
    ASSERT_EQ("0", live_encode->deviceName());
    ASSERT_EQ("0", live_decode0->deviceName());
    ASSERT_EQ("1", live_decode1->deviceName());
    live_decode0.reset();
    ASSERT_TRUE(transform().success);
    EXPECT_FALSE(usesGpuDecode(_runner->ffmpeg_argv));
    EXPECT_TRUE(_runner->hasArgs("-gpu", "1"));
}

TEST_F(TierTranscodeGpuTest, FullGpuBudgetUsesTheCpu) {
    ConfigGuard budget(mediakit::HwCodec::kEncodeBudget, "cuda:100");
    probeBackends({HwBackend::Cuda}, false);
    // 30 live + 60 offline > 100 * 0.8.
    auto live = HwCodecManager::Instance().acquireEncode(mediakit::CodecH264, 1920, 1080, 30);
    ASSERT_TRUE(live);
    ASSERT_TRUE(transform().success);
    ASSERT_EQ(1U, _runner->ffmpeg_runs.size());
    EXPECT_EQ("libx264", FakeProcessRunner::argAfter(_runner->ffmpeg_argv, "-c:v"));
}

TEST_F(TierTranscodeGpuTest, HwaccelOffUsesTheCpu) {
    ConfigGuard off(mediakit::Storage::kTierTranscodeHwaccel, "0");
    probeBackends({HwBackend::Cuda}, true);
    ASSERT_TRUE(transform().success);
    ASSERT_EQ(1U, _runner->ffmpeg_runs.size());
    EXPECT_EQ("libx264", FakeProcessRunner::argAfter(_runner->ffmpeg_argv, "-c:v"));
}
#endif
