#include <gtest/gtest.h>

#include "Common/MediaSource.h"

using namespace mediakit;

namespace {

class TestMediaSource : public MediaSource {
public:
    TestMediaSource(const std::string &schema, const MediaTuple &tuple, int readers = 0)
        : MediaSource(schema, tuple), readers(readers) {}

    int readerCount() override { return readers; }
    void registerSource() { regist(); }
    int readers;
};

class CapturingMediaEvent : public MediaSourceEvent {
public:
    MediaOriginType origin = MediaOriginType::pull;
    std::string origin_url = "origin://camera";
    int readers = 3;
    size_t registrations = 0;
    size_t unregistrations = 0;
    size_t close_calls = 0;
    uint32_t seek_stamp = 0;
    bool paused = false;
    float playback_speed = 1.0F;

    MediaOriginType getOriginType(MediaSource &) const override { return origin; }
    std::string getOriginUrl(MediaSource &) const override { return origin_url; }
    bool seekTo(MediaSource &, uint32_t stamp) override {
        seek_stamp = stamp;
        return true;
    }
    bool pause(MediaSource &, bool value) override {
        paused = value;
        return true;
    }
    bool speed(MediaSource &, float value) override {
        playback_speed = value;
        return true;
    }
    bool close(MediaSource &) override {
        ++close_calls;
        return true;
    }
    int totalReaderCount(MediaSource &) override { return readers; }
    void onRegist(MediaSource &, bool value) override {
        value ? ++registrations : ++unregistrations;
    }
    float getLossRate(MediaSource &, TrackType type) override {
        return type == TrackVideo ? 0.25F : 0.5F;
    }
};

} // namespace

TEST(MediaInfoTest, ParsesUrlsPortsNestedStreamsAndVhostOverride) {
    MediaInfo info("rtsp://example.com:8554/live/camera/sub?token=x&vhost=tenant.local");
    EXPECT_EQ("rtsp", info.schema);
    EXPECT_EQ("example.com", info.host);
    EXPECT_EQ(8554, info.port);
    // The test configuration has virtual hosts disabled, so every host is
    // normalized after parsing (including an explicit vhost query argument).
    EXPECT_EQ(DEFAULT_VHOST, info.vhost);
    EXPECT_EQ("live", info.app);
    EXPECT_EQ("camera/sub", info.stream);
    EXPECT_EQ("token=x&vhost=tenant.local", info.params);
    EXPECT_EQ(std::string("rtsp://") + DEFAULT_VHOST + "/live/camera/sub", info.getUrl());

    MediaInfo local("http://127.0.0.1/app/id");
    EXPECT_EQ(DEFAULT_VHOST, local.vhost);
    EXPECT_EQ("app", local.app);
    EXPECT_EQ("id", local.stream);

    MediaInfo schemeless("host.example/app/stream");
    EXPECT_TRUE(schemeless.schema.empty());
    EXPECT_EQ("host.example", schemeless.host);
}

TEST(MediaSourceTest, ExposesDefaultsAndDelegatesPlaybackControls) {
    auto source = std::make_shared<TestMediaSource>("rtsp", MediaTuple("tenant", "live", "cam", ""), 2);
    EXPECT_EQ(std::string("rtsp://") + DEFAULT_VHOST + "/live/cam", source->getUrl());
    EXPECT_EQ(MediaOriginType::unknown, source->getOriginType());
    EXPECT_EQ(source->getUrl(), source->getOriginUrl());
    EXPECT_EQ(2, source->totalReaderCount());
    EXPECT_FALSE(source->seekTo(10));
    EXPECT_FALSE(source->pause(true));
    EXPECT_FALSE(source->speed(2));
    EXPECT_FALSE(source->close(true));
    EXPECT_FLOAT_EQ(-1, source->getLossRate(TrackVideo));
    EXPECT_THROW(source->getOwnerPoller(), std::runtime_error);

    auto listener = std::make_shared<CapturingMediaEvent>();
    source->setListener(listener);
    EXPECT_EQ(MediaOriginType::pull, source->getOriginType());
    EXPECT_EQ("origin://camera", source->getOriginUrl());
    EXPECT_EQ(3, source->totalReaderCount());
    EXPECT_TRUE(source->seekTo(1234));
    EXPECT_TRUE(source->pause(true));
    EXPECT_TRUE(source->speed(1.5F));
    EXPECT_EQ(1234U, listener->seek_stamp);
    EXPECT_TRUE(listener->paused);
    EXPECT_FLOAT_EQ(1.5F, listener->playback_speed);
    EXPECT_FLOAT_EQ(0.25F, source->getLossRate(TrackVideo));
    EXPECT_FALSE(source->close(false));
    EXPECT_TRUE(source->close(true));
    EXPECT_EQ(1U, listener->close_calls);
}

TEST(MediaSourceTest, RegistersFindsTraversesAndRejectsDuplicates) {
    auto listener = std::make_shared<CapturingMediaEvent>();
    auto source = std::make_shared<TestMediaSource>("unit", MediaTuple("tenant", "app", "stream", ""));
    source->setListener(listener);
    source->registerSource();
    EXPECT_EQ(1U, listener->registrations);
    EXPECT_EQ(source, MediaSource::find("unit", DEFAULT_VHOST, "app", "stream", false));

    size_t matches = 0;
    MediaSource::for_each_media([&](const MediaSource::Ptr &item) {
        EXPECT_EQ(source, item);
        ++matches;
    }, "unit", DEFAULT_VHOST, "app", "stream");
    EXPECT_EQ(1U, matches);

    auto duplicate = std::make_shared<TestMediaSource>("unit", MediaTuple("tenant", "app", "stream", ""));
    EXPECT_THROW(duplicate->registerSource(), std::invalid_argument);
    source->registerSource();

    source.reset();
    EXPECT_FALSE(MediaSource::find("unit", DEFAULT_VHOST, "app", "stream", false));
    EXPECT_EQ(1U, listener->unregistrations);
}

TEST(MediaSourceTest, GrantsExclusiveOwnershipUntilTokenReleased) {
    auto source = std::make_shared<TestMediaSource>("unit", MediaTuple("tenant", "app", "owned", ""));
    auto first = source->getOwnership();
    ASSERT_TRUE(first);
    EXPECT_FALSE(source->getOwnership());
    first.reset();
    EXPECT_TRUE(source->getOwnership());
}

TEST(MediaSourceEventInterceptorTest, DelegatesAndFallsBack) {
    auto source = std::make_shared<TestMediaSource>("unit", MediaTuple("tenant", "app", "events", ""));
    auto interceptor = std::make_shared<MediaSourceEventInterceptor>();
    EXPECT_EQ(MediaOriginType::unknown, interceptor->getOriginType(*source));
    EXPECT_EQ(source->getUrl(), interceptor->getOriginUrl(*source));
    EXPECT_FALSE(interceptor->seekTo(*source, 10));
    EXPECT_THROW(interceptor->totalReaderCount(*source), MediaSourceEvent::NotImplemented);

    auto delegate = std::make_shared<CapturingMediaEvent>();
    interceptor->setDelegate(delegate);
    EXPECT_EQ(delegate, interceptor->getDelegate());
    EXPECT_EQ(MediaOriginType::pull, interceptor->getOriginType(*source));
    EXPECT_EQ("origin://camera", interceptor->getOriginUrl(*source));
    EXPECT_TRUE(interceptor->seekTo(*source, 99));
    EXPECT_TRUE(interceptor->pause(*source, true));
    EXPECT_TRUE(interceptor->speed(*source, 2.0F));
    EXPECT_TRUE(interceptor->close(*source));
    EXPECT_EQ(3, interceptor->totalReaderCount(*source));
    EXPECT_FLOAT_EQ(0.5F, interceptor->getLossRate(*source, TrackAudio));
    EXPECT_THROW(interceptor->setDelegate(interceptor), std::invalid_argument);

    delegate->origin_url.clear();
    EXPECT_EQ(source->getUrl(), interceptor->getOriginUrl(*source));
    delegate.reset();
    EXPECT_FALSE(interceptor->getDelegate());
}

TEST(MediaSourceHelpersTest, ConvertsOriginsAndComparesTuples) {
    EXPECT_EQ("unknown", getOriginTypeString(MediaOriginType::unknown));
    EXPECT_EQ("rtmp_push", getOriginTypeString(MediaOriginType::rtmp_push));
    EXPECT_EQ("rtsp_push", getOriginTypeString(MediaOriginType::rtsp_push));
    EXPECT_EQ("rtp_push", getOriginTypeString(MediaOriginType::rtp_push));
    EXPECT_EQ("pull", getOriginTypeString(MediaOriginType::pull));
    EXPECT_EQ("ffmpeg_pull", getOriginTypeString(MediaOriginType::ffmpeg_pull));
    EXPECT_EQ("mp4_vod", getOriginTypeString(MediaOriginType::mp4_vod));
    EXPECT_EQ("device_chn", getOriginTypeString(MediaOriginType::device_chn));
    EXPECT_EQ("rtc_push", getOriginTypeString(MediaOriginType::rtc_push));
    EXPECT_EQ("srt_push", getOriginTypeString(MediaOriginType::srt_push));
    EXPECT_EQ("unknown", getOriginTypeString(static_cast<MediaOriginType>(255)));

    EXPECT_TRUE(equalMediaTuple(MediaTuple("v", "a", "s", "x"), MediaTuple("v", "a", "s", "y")));
    EXPECT_FALSE(equalMediaTuple(MediaTuple("v", "a", "one"), MediaTuple("v", "a", "two")));
}

TEST(ProtocolOptionTest, LoadsTypedValuesFromIni) {
    toolkit::mINI args;
    args["modify_stamp"] = "2";
    args["enable_audio"] = "0";
    args["add_mute_audio"] = "0";
    args["auto_close"] = "1";
    args["continue_push_ms"] = "1500";
    args["paced_sender_ms"] = "10";
    args["enable_hls"] = "0";
    args["enable_mp4"] = "1";
    args["enable_rtsp"] = "1";
    args["enable_rtmp"] = "0";
    args["hls_demand"] = "1";
    args["mp4_max_second"] = "60";
    args["mp4_as_player"] = "1";
    args["mp4_save_path"] = "/tmp/mp4";
    args["hls_save_path"] = "/tmp/hls";
    args["stream_replace"] = "replacement";
    args["max_track"] = "3";
    args["enable_motion"] = "1";
    args["roi_mask"] = "mask";
    args["record_motion"] = "1";
    args["pre_record_ms"] = "2000";
    args["post_record_ms"] = "3000";
    args["motion_demand"] = "1";
    args["enable_gop_cache"] = "1";
    args["gop_cache_size"] = "4";

    ProtocolOption option(args);
    EXPECT_EQ(ProtocolOption::kModifyStampRelative, option.modify_stamp);
    EXPECT_FALSE(option.enable_audio);
    EXPECT_FALSE(option.add_mute_audio);
    EXPECT_TRUE(option.auto_close);
    EXPECT_EQ(1500U, option.continue_push_ms);
    EXPECT_EQ(10U, option.paced_sender_ms);
    EXPECT_FALSE(option.enable_hls);
    EXPECT_TRUE(option.enable_mp4);
    EXPECT_TRUE(option.enable_rtsp);
    EXPECT_FALSE(option.enable_rtmp);
    EXPECT_TRUE(option.hls_demand);
    EXPECT_EQ(60U, option.mp4_max_second);
    EXPECT_TRUE(option.mp4_as_player);
    EXPECT_EQ("/tmp/mp4", option.mp4_save_path);
    EXPECT_EQ("/tmp/hls", option.hls_save_path);
    EXPECT_EQ("replacement", option.stream_replace);
    EXPECT_EQ(3U, option.max_track);
    EXPECT_TRUE(option.enable_motion);
    EXPECT_EQ("mask", option.roi_mask);
    EXPECT_TRUE(option.record_motion);
    EXPECT_EQ(2000U, option.pre_record_ms);
    EXPECT_EQ(3000U, option.post_record_ms);
    EXPECT_TRUE(option.motion_demand);
    EXPECT_TRUE(option.enable_gop_cache);
    EXPECT_EQ(4, option.gop_cache_size);
}
