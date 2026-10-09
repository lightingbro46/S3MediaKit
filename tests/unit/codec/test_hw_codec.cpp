#include <gtest/gtest.h>

#if defined(ENABLE_FFMPEG)

#include <csignal>
#include <cstring>
#include <unistd.h>
#include "Codec/HwCodec.h"
#include "Codec/Transcode.h"
#include "Common/config.h"
#include "Extension/Factory.h"

using namespace mediakit;
using namespace toolkit;

namespace {

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

struct FakeBackend {
    HwBackend backend;
    HwBackendCaps caps;
};

// Probe that reports the given backends as working, without any device context.
HwCodecManager::ProbeFunc fakeProbe(std::vector<FakeBackend> backends, std::vector<std::string> *devices = nullptr) {
    return [backends, devices](HwBackend backend, const std::string &device, HwBackendCaps &caps, AVBufferRef *&) {
        if (devices) {
            devices->emplace_back(std::string(hwBackendName(backend)) + "=" + device);
        }
        for (auto &item : backends) {
            if (item.backend == backend) {
                caps = item.caps;
                return true;
            }
        }
        return false;
    };
}

HwBackendCaps makeCaps(bool dec264, bool dec265, bool enc264, bool enc265) {
    HwBackendCaps caps;
    caps.decode[0] = dec264;
    caps.decode[1] = dec265;
    caps.encode[0] = enc264;
    caps.encode[1] = enc265;
    caps.hw_pix_fmt = AV_PIX_FMT_CUDA;
    return caps;
}

class HwCodecManagerTest : public ::testing::Test {
protected:
    ConfigGuard _enable { HwCodec::kEnable, "1" };
    ConfigGuard _decode { HwCodec::kDecode, "1" };
    ConfigGuard _encode { HwCodec::kEncode, "1" };
    ConfigGuard _nvidia { General::kCheckNvidiaDev, "1" };
    ConfigGuard _backends { HwCodec::kBackends, "cuda,vaapi,qsv" };
    ConfigGuard _max_dec { HwCodec::kMaxDecodeSessions, "0" };
    ConfigGuard _max_enc { HwCodec::kMaxEncodeSessions, "0" };
    ConfigGuard _threshold { HwCodec::kFailThreshold, "2" };
    ConfigGuard _cooldown { HwCodec::kCooldownSec, "300" };
    ConfigGuard _device { HwCodec::kDevice, "" };
    ConfigGuard _dec_budget { HwCodec::kDecodeBudget, "" };
    ConfigGuard _enc_budget { HwCodec::kEncodeBudget, "" };
    ConfigGuard _offline_cost { HwCodec::kOfflineCost, "60" };
    ConfigGuard _offline_headroom { HwCodec::kOfflineHeadroom, "0.8" };
};

} // namespace

TEST_F(HwCodecManagerTest, NothingBeforeProbe) {
    HwCodecManager manager;
    EXPECT_FALSE(manager.probed());
    EXPECT_FALSE(manager.acquireDecode(CodecH264));
    EXPECT_FALSE(manager.acquireEncode(CodecH264));
}

TEST_F(HwCodecManagerTest, PicksFirstConfiguredBackendWithCapability) {
    HwCodecManager manager;
    std::vector<std::string> devices;
    ConfigGuard device(HwCodec::kDevice, "cuda:1,/dev/dri/renderD129");
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, false, true, false) },
                              { HwBackend::Vaapi, makeCaps(true, true, false, false) } }, &devices));
    ASSERT_TRUE(manager.probed());
    EXPECT_EQ("cuda,vaapi", manager.backends());
    EXPECT_EQ((std::vector<std::string> { "cuda=1", "vaapi=/dev/dri/renderD129", "qsv=/dev/dri/renderD129" }), devices);

    auto dec264 = manager.acquireDecode(CodecH264);
    ASSERT_TRUE(dec264);
    EXPECT_EQ(HwBackend::Cuda, dec264->backend());
    auto dec265 = manager.acquireDecode(CodecH265);
    ASSERT_TRUE(dec265);
    EXPECT_EQ(HwBackend::Vaapi, dec265->backend());
    EXPECT_FALSE(manager.acquireEncode(CodecH265));
    EXPECT_FALSE(manager.acquireDecode(CodecAAC));
    EXPECT_EQ(2U, manager.hwDecoderCount());

    {
        // Hot reload reorders the backends.
        ConfigGuard order(HwCodec::kBackends, "vaapi,cuda");
        auto dec = manager.acquireDecode(CodecH264);
        ASSERT_TRUE(dec);
        EXPECT_EQ(HwBackend::Vaapi, dec->backend());
    }
    {
        // general.check_nvidia_dev = 0 still means "never cuda".
        ConfigGuard nvidia(General::kCheckNvidiaDev, "0");
        auto dec = manager.acquireDecode(CodecH264);
        ASSERT_TRUE(dec);
        EXPECT_EQ(HwBackend::Vaapi, dec->backend());
        EXPECT_FALSE(manager.acquireEncode(CodecH264));
    }
    dec264.reset();
    dec265.reset();
    EXPECT_EQ(0U, manager.hwDecoderCount());
}

TEST_F(HwCodecManagerTest, DisabledByConfig) {
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, true, true) } }));
    {
        ConfigGuard decode(HwCodec::kDecode, "0");
        EXPECT_FALSE(manager.acquireDecode(CodecH264));
        EXPECT_TRUE(manager.acquireEncode(CodecH264));
    }
    {
        ConfigGuard enable(HwCodec::kEnable, "0");
        EXPECT_FALSE(manager.acquireDecode(CodecH264));
        EXPECT_FALSE(manager.acquireEncode(CodecH264));
    }
    {
        // Disabled at probe time: no device is created at all.
        ConfigGuard enable(HwCodec::kEnable, "0");
        HwCodecManager off;
        bool called = false;
        off.probe([&called](HwBackend, const std::string &, HwBackendCaps &, AVBufferRef *&) {
            called = true;
            return true;
        });
        EXPECT_FALSE(called);
        EXPECT_TRUE(off.probed());
        EXPECT_EQ("", off.backends());
    }
}

TEST_F(HwCodecManagerTest, SessionBudgetSpillsToNextBackendThenCpu) {
    ConfigGuard max_dec(HwCodec::kMaxDecodeSessions, "1");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, false, false) },
                              { HwBackend::Vaapi, makeCaps(true, true, false, false) } }));
    auto first = manager.acquireDecode(CodecH264);
    auto second = manager.acquireDecode(CodecH265);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(HwBackend::Cuda, first->backend());
    EXPECT_EQ(HwBackend::Vaapi, second->backend());
    EXPECT_FALSE(manager.acquireDecode(CodecH264));
    first.reset();
    auto third = manager.acquireDecode(CodecH264);
    ASSERT_TRUE(third);
    EXPECT_EQ(HwBackend::Cuda, third->backend());
}

TEST_F(HwCodecManagerTest, BreakerOpensAfterFailedSessions) {
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, true, true) } }));
    for (int i = 0; i < 2; ++i) {
        auto session = manager.acquireDecode(CodecH264);
        ASSERT_TRUE(session);
        session->reportFailure();
        session->reportFailure(); // counted once per session
    }
    EXPECT_FALSE(manager.acquireDecode(CodecH264));
    EXPECT_FALSE(manager.acquireEncode(CodecH264));
}

TEST_F(HwCodecManagerTest, SuccessResetsFailureCount) {
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, true, true) } }));
    manager.acquireDecode(CodecH264)->reportFailure();
    manager.acquireDecode(CodecH264)->reportSuccess();
    manager.acquireDecode(CodecH264)->reportFailure();
    EXPECT_TRUE(manager.acquireDecode(CodecH264));
}

TEST_F(HwCodecManagerTest, HalfOpenAllowsOneTrial) {
    ConfigGuard cooldown(HwCodec::kCooldownSec, "0");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, true, true) } }));
    manager.acquireDecode(CodecH264)->reportFailure();
    manager.acquireDecode(CodecH264)->reportFailure();

    // Cooldown over: one trial at a time.
    auto trial = manager.acquireDecode(CodecH264);
    ASSERT_TRUE(trial);
    EXPECT_FALSE(manager.acquireDecode(CodecH264));
    // A trial that ends without a verdict lets the next one try.
    trial.reset();
    trial = manager.acquireDecode(CodecH264);
    ASSERT_TRUE(trial);
    EXPECT_FALSE(manager.acquireDecode(CodecH264));
    // A failed trial reopens the breaker (cooldown 0: the next acquire is a new trial).
    trial->reportFailure();
    auto trial2 = manager.acquireDecode(CodecH264);
    ASSERT_TRUE(trial2);
    EXPECT_FALSE(manager.acquireDecode(CodecH264));
    // A working trial closes it.
    trial2->reportSuccess();
    auto a = manager.acquireDecode(CodecH264);
    auto b = manager.acquireDecode(CodecH264);
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
}

TEST_F(HwCodecManagerTest, EncoderOpenFailureWithOthersRunningLearnsSessionLimit) {
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(false, false, true, true) } }));
    auto a = manager.acquireEncode(CodecH264);
    auto b = manager.acquireEncode(CodecH264);
    auto c = manager.acquireEncode(CodecH265);
    ASSERT_TRUE(a && b && c);
    c->reportFailure(true);
    c.reset();
    // Capped at the two that run; the breaker is not involved.
    EXPECT_FALSE(manager.acquireEncode(CodecH264));
    EXPECT_EQ(2U, manager.hwEncoderCount());
    b.reset();
    auto d = manager.acquireEncode(CodecH264);
    EXPECT_TRUE(d);
    // An open failure with nothing else running is a real failure.
    a.reset();
    d.reset();
    manager.acquireEncode(CodecH264)->reportFailure(true);
    manager.acquireEncode(CodecH264)->reportFailure(true);
    EXPECT_FALSE(manager.acquireEncode(CodecH264));
}

TEST_F(HwCodecManagerTest, LoadBudgetSpillsToNextBackendThenCpu) {
    ConfigGuard budget(HwCodec::kDecodeBudget, "cuda:60,vaapi:30");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, false, false) },
                              { HwBackend::Vaapi, makeCaps(true, true, false, false) } }));
    auto a = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    auto b = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    auto c = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(HwBackend::Cuda, a->backend());
    EXPECT_EQ(HwBackend::Cuda, b->backend());
    EXPECT_EQ(HwBackend::Vaapi, c->backend());
    EXPECT_FALSE(manager.acquireDecode(CodecH264, 1920, 1080, 30));
    EXPECT_EQ("cuda dec=60/60 enc=0,vaapi dec=30/30 enc=0", manager.loads());
    // A closed session gives its load back.
    a.reset();
    auto d = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    ASSERT_TRUE(d);
    EXPECT_EQ(HwBackend::Cuda, d->backend());
}

TEST_F(HwCodecManagerTest, SmallStreamCostsLessThan1080p) {
    ConfigGuard budget(HwCodec::kDecodeBudget, "cuda:30");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, false, false) } }));
    auto sub = manager.acquireDecode(CodecH264, 640, 360, 15);
    ASSERT_TRUE(sub);
    EXPECT_NEAR(640.0 * 360 * 15 / (1920 * 1080), sub->cost(), 1e-9);
    // 1.67 + 30 > 30: a 1080p30 stream no longer fits, more Sub streams do.
    EXPECT_FALSE(manager.acquireDecode(CodecH264, 1920, 1080, 30));
    EXPECT_TRUE(manager.acquireDecode(CodecH264, 640, 360, 15));
}

TEST_F(HwCodecManagerTest, UnknownSizeAssumes1080p25UntilUpdated) {
    ConfigGuard budget(HwCodec::kDecodeBudget, "cuda:40");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, false, false) } }));
    auto a = manager.acquireDecode(CodecH264);
    ASSERT_TRUE(a);
    EXPECT_DOUBLE_EQ(25, a->cost());
    EXPECT_FALSE(manager.acquireDecode(CodecH264, 1920, 1080, 30));
    // The first decoded frame tells the real size.
    a->updateCost(640, 360, 15);
    auto b = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    ASSERT_TRUE(b);
    // A resolution change moves the load again; the running sessions stay even above the budget.
    a->updateCost(3840, 2160, 30);
    EXPECT_EQ("cuda dec=150/40 enc=0", manager.loads());
    EXPECT_FALSE(manager.acquireDecode(CodecH264, 640, 360, 15));
    a.reset();
    EXPECT_EQ("cuda dec=30/40 enc=0", manager.loads());
}

TEST_F(HwCodecManagerTest, BudgetOnlyForListedBackends) {
    ConfigGuard budget(HwCodec::kDecodeBudget, "vaapi:30");
    ConfigGuard order(HwCodec::kBackends, "vaapi,cuda");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, false, false) },
                              { HwBackend::Vaapi, makeCaps(true, true, false, false) } }));
    auto a = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    ASSERT_TRUE(a);
    EXPECT_EQ(HwBackend::Vaapi, a->backend());
    // cuda has no budget: no load limit.
    for (int i = 0; i < 20; ++i) {
        auto b = manager.acquireDecode(CodecH264, 3840, 2160, 60);
        ASSERT_TRUE(b);
        EXPECT_EQ(HwBackend::Cuda, b->backend());
    }
}

TEST_F(HwCodecManagerTest, OfflineEncodeLeavesHeadroomForLive) {
    ConfigGuard budget(HwCodec::kEncodeBudget, "cuda:100");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(false, false, true, true) } }));
    auto extract = manager.acquireEncodeOffline(CodecH264);
    ASSERT_TRUE(extract);
    EXPECT_DOUBLE_EQ(60, extract->cost());
    // 60 + 60 > 100 * 0.8.
    EXPECT_FALSE(manager.acquireEncodeOffline(CodecH264));
    // Live may use the full budget.
    auto live = manager.acquireEncode(CodecH264, 1920, 1080, 30);
    ASSERT_TRUE(live);
    EXPECT_FALSE(manager.acquireEncode(CodecH264, 1920, 1080, 30));
    live.reset();
    extract.reset();
    {
        // Without a budget an offline job is one more session, as before.
        ConfigGuard none(HwCodec::kEncodeBudget, "");
        auto x = manager.acquireEncodeOffline(CodecH264);
        auto y = manager.acquireEncodeOffline(CodecH264);
        EXPECT_TRUE(x && y);
    }
}

TEST_F(HwCodecManagerTest, LoweringBudgetKeepsRunningSessions) {
    ConfigGuard budget(HwCodec::kDecodeBudget, "cuda:60");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, false, false) } }));
    auto a = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    auto b = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    ASSERT_TRUE(a && b);
    {
        ConfigGuard lower(HwCodec::kDecodeBudget, "cuda:30");
        EXPECT_EQ("cuda dec=60/30 enc=0", manager.loads());
        EXPECT_FALSE(manager.acquireDecode(CodecH264, 640, 360, 15));
    }
    a.reset();
    EXPECT_TRUE(manager.acquireDecode(CodecH264, 1920, 1080, 30));
}

TEST_F(HwCodecManagerTest, SeveralDevicesOfOneBackend) {
    ConfigGuard device(HwCodec::kDevice, "cuda:0; 1");
    HwCodecManager manager;
    std::vector<std::string> devices;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, true, true) } }, &devices));
    EXPECT_EQ((std::vector<std::string> { "cuda=0", "cuda=1", "vaapi=", "qsv=" }), devices);
    EXPECT_EQ("cuda", manager.backends());

    // Least loaded device first.
    auto a = manager.acquireDecode(CodecH264, 1920, 1080, 30);
    auto b = manager.acquireDecode(CodecH264, 640, 360, 15);
    auto c = manager.acquireDecode(CodecH264, 640, 360, 15);
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ("0", a->deviceName());
    EXPECT_EQ("1", b->deviceName());
    EXPECT_EQ("1", c->deviceName());
    EXPECT_EQ("cuda:0 dec=30 enc=0,cuda:1 dec=3 enc=0", manager.loads());
    {
        // Budget per device.
        ConfigGuard budget(HwCodec::kDecodeBudget, "cuda:32");
        auto d = manager.acquireDecode(CodecH264, 1920, 1080, 25);
        ASSERT_TRUE(d);
        EXPECT_EQ("1", d->deviceName());
        EXPECT_FALSE(manager.acquireDecode(CodecH264, 1920, 1080, 25));
    }
    a.reset();
    b.reset();
    c.reset();

    // A device whose breaker is open is skipped, the other one keeps working.
    for (int i = 0; i < 2; ++i) {
        auto keep = manager.acquireDecode(CodecH264); // device 0, the next one goes to device 1
        auto other = manager.acquireDecode(CodecH264);
        ASSERT_TRUE(keep && other);
        ASSERT_EQ("1", other->deviceName());
        other->reportFailure();
    }
    // Held open: device 1 would be the least loaded from the second session on.
    std::vector<HwCodecSession::Ptr> held;
    for (int i = 0; i < 3; ++i) {
        auto session = manager.acquireDecode(CodecH264);
        ASSERT_TRUE(session);
        EXPECT_EQ("0", session->deviceName());
        held.emplace_back(session);
    }
}

TEST_F(HwCodecManagerTest, EncoderSessionLimitCountsEveryDevice) {
    // Consumer NVIDIA cards limit NVENC sessions for the whole system, not per card.
    ConfigGuard device(HwCodec::kDevice, "cuda:0;1");
    HwCodecManager manager;
    manager.probe(fakeProbe({ { HwBackend::Cuda, makeCaps(false, false, true, true) } }));
    auto a = manager.acquireEncode(CodecH264);
    auto b = manager.acquireEncode(CodecH264);
    auto c = manager.acquireEncode(CodecH264);
    ASSERT_TRUE(a && b && c);
    EXPECT_NE(a->deviceName(), b->deviceName());
    c->reportFailure(true);
    c.reset();
    // Capped at the two running, on either device.
    EXPECT_FALSE(manager.acquireEncode(CodecH264));
    a.reset();
    EXPECT_TRUE(manager.acquireEncode(CodecH264));
}

namespace {

FFmpegFrame::Ptr makeYuvFrame(int width, int height, int index) {
    auto frame = std::make_shared<FFmpegFrame>();
    auto avf = frame->get();
    avf->format = AV_PIX_FMT_YUV420P;
    avf->width = width;
    avf->height = height;
    if (av_frame_get_buffer(avf, 32) < 0) {
        return nullptr;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            avf->data[0][y * avf->linesize[0] + x] = (uint8_t)(x + y + index * 4);
        }
    }
    for (int y = 0; y < height / 2; ++y) {
        memset(avf->data[1] + y * avf->linesize[1], 128, width / 2);
        memset(avf->data[2] + y * avf->linesize[2], 128, width / 2);
    }
    avf->pts = index * 40;
    return frame;
}

// Encodes `count` frames with FFmpegEncoder and decodes them with FFmpegDecoder.
size_t encodeThenDecode(int count, std::vector<int> &formats, CodecId codec = CodecH264, int gop = 25,
                        const std::vector<std::string> &decoder_names = {}, std::string *decoder_name = nullptr) {
    std::vector<Frame::Ptr> encoded;
    FFmpegEncoder encoder(codec, 320, 240, 25, 400000, gop);
    encoder.setOnEncode([&encoded](const Frame::Ptr &frame) { encoded.emplace_back(Frame::getCacheAbleFrame(frame)); });
    for (int i = 0; i < count; ++i) {
        auto frame = makeYuvFrame(320, 240, i);
        if (!frame || !encoder.inputFrame(frame)) {
            return 0;
        }
    }
    encoder.flush();

    auto track = Factory::getTrackByCodecId(codec);
    size_t decoded = 0;
    FFmpegDecoder decoder(track, 0, decoder_names);
    decoder.setOnDecode([&](const FFmpegFrame::Ptr &frame) {
        ++decoded;
        formats.emplace_back(frame->get()->format);
    });
    for (auto &frame : encoded) {
        decoder.inputFrame(frame, false, false, false);
    }
    decoder.flush();
    if (decoder_name) {
        *decoder_name = decoder.getContext()->codec->name;
    }
    return decoded;
}

} // namespace

TEST_F(HwCodecManagerTest, DecoderFallsBackToCpuWhenGpuCannotDecode) {
    // A "cuda" backend without a device: the decoder gets a GPU session, FFmpeg cannot use the
    // CUDA surface format and decodes in software, the decoder notices and gives the session back.
    HwCodecManager::Instance().probe(fakeProbe({ { HwBackend::Cuda, makeCaps(true, true, false, false) } }));
    auto fallbacks = HwCodecManager::Instance().fallbackCount();
    std::vector<int> formats;
    EXPECT_EQ(10U, encodeThenDecode(10, formats));
    for (auto fmt : formats) {
        EXPECT_EQ(AV_PIX_FMT_YUV420P, fmt);
    }
    EXPECT_EQ(fallbacks + 1, HwCodecManager::Instance().fallbackCount());
    EXPECT_EQ(0U, HwCodecManager::Instance().hwDecoderCount());
    HwCodecManager::Instance().probe(fakeProbe({}));
}

TEST_F(HwCodecManagerTest, NamedDecoderListMatchesTrackCodec) {
    // {"h264", "hevc"} used to give the h264 decoder to H265 tracks too.
    std::vector<int> formats;
    std::string name;
    HwCodecManager::Instance().probe(fakeProbe({}));
    EXPECT_EQ(10U, encodeThenDecode(10, formats, CodecH265, 25, { "h264", "hevc" }, &name));
    EXPECT_EQ("hevc", name);
}

TEST_F(HwCodecManagerTest, BrokenWrapperDecoderFallsBackToNative) {
    // The old default list picked hevc_qsv before the native decoder: it opens fine without an Intel
    // GPU and then fails every packet, so HEVC streams gave no frame at all.
    if (!avcodec_find_decoder_by_name("hevc_qsv")) {
        GTEST_SKIP() << "FFmpeg built without hevc_qsv";
    }
    HwCodecManager::Instance().probe(fakeProbe({}));
    auto fallbacks = HwCodecManager::Instance().fallbackCount();
    std::vector<int> formats;
    std::string name;
    // 30 frames, keyframe every 10: after the switch the native decoder starts at the next keyframe.
    auto decoded = encodeThenDecode(30, formats, CodecH265, 10, { "hevc_qsv" }, &name);
    EXPECT_GE(decoded, 10U);
    for (auto fmt : formats) {
        EXPECT_EQ(AV_PIX_FMT_YUV420P, fmt);
    }
    if (name == "hevc") {
        EXPECT_EQ(fallbacks + 1, HwCodecManager::Instance().fallbackCount());
    }
}

TEST_F(HwCodecManagerTest, RealProbeKeepsCpuPathWorking) {
    // On a machine without GPU every backend fails to probe and the CPU path stays as before;
    // with a GPU the frames still come back as YUV420P in RAM.
    HwCodecManager::Instance().probe();
    EXPECT_TRUE(HwCodecManager::Instance().probed());
    for (auto codec : { CodecH264, CodecH265 }) {
        std::vector<int> formats;
        EXPECT_EQ(10U, encodeThenDecode(10, formats, codec)) << getCodecName(codec);
        for (auto fmt : formats) {
            EXPECT_EQ(AV_PIX_FMT_YUV420P, fmt);
        }
    }
    HwCodecManager::Instance().probe(fakeProbe({}));
}

#if defined(__linux__) && !defined(__ANDROID__)
TEST(HwChildTestRunner, CrashOrHangDisablesOnlyThatTest) {
    std::vector<std::pair<std::string, HwChildTest>> tests;
    tests.emplace_back("a", [](std::string &detail) {
        detail = "fine";
        return true;
    });
    tests.emplace_back("crash", [](std::string &) {
        raise(SIGSEGV);
        return true;
    });
    tests.emplace_back("b", [](std::string &detail) {
        detail = "not supported";
        return false;
    });
    tests.emplace_back("hang", [](std::string &) {
        sleep(30);
        return true;
    });
    tests.emplace_back("c", [](std::string &) { return true; });

    std::map<std::string, HwTestResult> results;
    ASSERT_TRUE(runHwTestsInChildren([](std::string &) { return true; }, tests, 1000, results));
    EXPECT_TRUE(results["setup"].ok);
    EXPECT_TRUE(results["a"].ok);
    EXPECT_EQ("fine", results["a"].detail);
    EXPECT_FALSE(results["crash"].ok);
    EXPECT_NE(std::string::npos, results["crash"].detail.find("signal 11")) << results["crash"].detail;
    EXPECT_FALSE(results["b"].ok);
    EXPECT_EQ("not supported", results["b"].detail);
    EXPECT_FALSE(results["hang"].ok);
    EXPECT_NE(std::string::npos, results["hang"].detail.find("hung")) << results["hang"].detail;
    EXPECT_TRUE(results["c"].ok);
}

TEST(HwChildTestRunner, FailedOrCrashedSetupStopsEverything) {
    std::vector<std::pair<std::string, HwChildTest>> tests;
    tests.emplace_back("a", [](std::string &) { return true; });
    std::map<std::string, HwTestResult> results;
    ASSERT_TRUE(runHwTestsInChildren([](std::string &detail) {
        detail = "no device";
        return false;
    }, tests, 1000, results));
    EXPECT_FALSE(results["setup"].ok);
    EXPECT_EQ("no device", results["setup"].detail);
    EXPECT_FALSE(results.count("a"));

    ASSERT_TRUE(runHwTestsInChildren([](std::string &) {
        abort();
        return true;
    }, tests, 1000, results));
    EXPECT_FALSE(results["setup"].ok);
    EXPECT_NE(std::string::npos, results["setup"].detail.find("signal 6")) << results["setup"].detail;
}
#endif

#endif // ENABLE_FFMPEG
