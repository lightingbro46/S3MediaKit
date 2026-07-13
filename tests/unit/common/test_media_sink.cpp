#include <gtest/gtest.h>

#include "Common/MediaSink.h"

using namespace mediakit;

namespace {

class CapturingMediaSink : public MediaSink {
public:
    std::vector<Track::Ptr> ready_tracks;
    std::vector<Frame::Ptr> frames;
    size_t all_ready_count = 0;

protected:
    bool onTrackReady(const Track::Ptr &track) override {
        ready_tracks.emplace_back(track);
        return true;
    }
    void onAllTrackReady() override { ++all_ready_count; }
    bool onTrackFrame(const Frame::Ptr &frame) override {
        frames.emplace_back(frame);
        return true;
    }
};

Frame::Ptr makeFrame(CodecId codec, int index, uint64_t dts) {
    static char payload[] = {0x01, 0x02, 0x03, 0x04};
    auto frame = std::make_shared<FrameFromPtr>(codec, payload, sizeof(payload), dts, dts);
    frame->setIndex(index);
    return frame;
}

class CapturingTrackListener : public TrackListener {
public:
    size_t added = 0;
    size_t completed = 0;
    size_t reset = 0;

    bool addTrack(const Track::Ptr &) override {
        ++added;
        return true;
    }
    void addTrackCompleted() override { ++completed; }
    void resetTracks() override { ++reset; }
};

class TestableMediaSinkDelegate : public MediaSinkDelegate {
public:
    void resetForTest() { resetTracks(); }
};

class TestableDemuxer : public Demuxer {
public:
    bool add(const Track::Ptr &track) { return addTrack(track); }
    void complete() { addTrackCompleted(); }
    void reset() { resetTracks(); }
};

} // namespace

TEST(MediaSinkTest, WaitsForTracksThenForwardsFrames) {
    CapturingMediaSink sink;
    sink.enableMuteAudio(false);
    auto video = std::make_shared<VideoTrackImp>(CodecH264, 1920, 1080, 25);
    auto audio = std::make_shared<AudioTrackImp>(CodecAAC, 48000, 2, 16);
    ASSERT_TRUE(sink.addTrack(video));
    ASSERT_TRUE(sink.addTrack(audio));
    EXPECT_FALSE(sink.isAllTrackReady());

    EXPECT_TRUE(sink.inputFrame(makeFrame(CodecH264, TrackVideo, 0)));
    EXPECT_FALSE(sink.isAllTrackReady());
    EXPECT_TRUE(sink.inputFrame(makeFrame(CodecAAC, TrackAudio, 0)));

    EXPECT_TRUE(sink.isAllTrackReady());
    EXPECT_TRUE(sink.haveVideo());
    EXPECT_EQ(2U, sink.ready_tracks.size());
    EXPECT_EQ(1U, sink.all_ready_count);
    EXPECT_GE(sink.frames.size(), 2U);
    EXPECT_FALSE(sink.addTrack(audio));
    EXPECT_TRUE(sink.inputFrame(makeFrame(CodecH264, TrackVideo, 40)));
}

TEST(MediaSinkTest, AppliesTrackPoliciesAndCanReset) {
    auto audio = std::make_shared<AudioTrackImp>(CodecAAC, 8000, 1, 16);
    auto video = std::make_shared<VideoTrackImp>(CodecH264, 640, 360, 25);

    CapturingMediaSink video_only;
    video_only.enableAudio(false);
    EXPECT_FALSE(video_only.addTrack(audio));
    EXPECT_TRUE(video_only.addTrack(video));
    EXPECT_TRUE(video_only.inputFrame(makeFrame(CodecH264, TrackVideo, 0)));
    EXPECT_TRUE(video_only.isAllTrackReady());
    EXPECT_FALSE(video_only.inputFrame(makeFrame(CodecAAC, TrackAudio, 0)));
    video_only.resetTracks();
    EXPECT_FALSE(video_only.isAllTrackReady());
    EXPECT_TRUE(video_only.getTracks(false).empty());

    CapturingMediaSink audio_only;
    audio_only.setOnlyAudio();
    EXPECT_FALSE(audio_only.addTrack(video));
    EXPECT_TRUE(audio_only.addTrack(audio));
    EXPECT_TRUE(audio_only.inputFrame(makeFrame(CodecAAC, TrackAudio, 0)));
    EXPECT_TRUE(audio_only.isAllTrackReady());
    EXPECT_FALSE(audio_only.haveVideo());
}

TEST(MediaSinkTest, EnforcesMaximumTrackCountAndDuplicateIndex) {
    CapturingMediaSink sink;
    sink.setMaxTrackCount(1);
    auto video = std::make_shared<VideoTrackImp>(CodecH264, 320, 240, 15);
    auto second_video = std::make_shared<VideoTrackImp>(CodecH265, 320, 240, 15);
    EXPECT_TRUE(sink.addTrack(video));
    EXPECT_FALSE(sink.addTrack(second_video));

    CapturingMediaSink duplicate;
    duplicate.setMaxTrackCount(2);
    EXPECT_TRUE(duplicate.addTrack(video));
    EXPECT_FALSE(duplicate.addTrack(second_video));
}

TEST(MuteAudioMakerTest, EmitsAtMostOneSilentFramePerInterval) {
    MuteAudioMaker maker;
    std::vector<Frame::Ptr> output;
    maker.addDelegate([&](const Frame::Ptr &frame) {
        output.emplace_back(frame);
        return true;
    });

    EXPECT_FALSE(maker.inputFrame(makeFrame(CodecH264, TrackVideo, 0)));
    EXPECT_TRUE(maker.inputFrame(makeFrame(CodecH264, TrackVideo, 128)));
    EXPECT_FALSE(maker.inputFrame(makeFrame(CodecH264, TrackVideo, 200)));
    EXPECT_FALSE(maker.inputFrame(makeFrame(CodecH264, 99, 256)));
    ASSERT_EQ(1U, output.size());
    EXPECT_EQ(CodecAAC, output[0]->getCodecId());
    EXPECT_EQ(128U, output[0]->dts());
}

TEST(MediaSinkDelegateTest, NotifiesListenerLifecycle) {
    TestableMediaSinkDelegate sink;
    CapturingTrackListener listener;
    sink.setTrackListener(&listener);
    sink.enableMuteAudio(false);
    auto audio = std::make_shared<AudioTrackImp>(CodecAAC, 8000, 1, 16);
    ASSERT_TRUE(sink.addTrack(audio));
    sink.addTrackCompleted();
    ASSERT_TRUE(sink.inputFrame(makeFrame(CodecAAC, TrackAudio, 0)));
    EXPECT_EQ(1U, listener.added);
    EXPECT_EQ(1U, listener.completed);
    sink.resetForTest();
    EXPECT_EQ(1U, listener.reset);
}

TEST(DemuxerTest, ForwardsTracksImmediatelyWithoutReadinessGate) {
    TestableDemuxer demuxer;
    CapturingTrackListener listener;
    demuxer.setTrackListener(&listener, false);
    auto audio = std::make_shared<AudioTrackImp>(CodecAAC, 16000, 1, 16);
    EXPECT_TRUE(demuxer.add(audio));
    demuxer.complete();
    EXPECT_EQ(1U, listener.added);
    EXPECT_EQ(1U, listener.completed);
    ASSERT_EQ(1U, demuxer.getTracks().size());
    EXPECT_EQ(TrackAudio, demuxer.getTrack(TrackAudio)->getTrackType());
    EXPECT_FALSE(demuxer.getTrack(TrackVideo));
    demuxer.reset();
    EXPECT_EQ(1U, listener.reset);
}

TEST(DemuxerTest, WaitsForTrackFramesWhenConfigured) {
    TestableDemuxer demuxer;
    CapturingTrackListener listener;
    demuxer.setTrackListener(&listener, true);
    auto audio = std::make_shared<AudioTrackImp>(CodecAAC, 8000, 1, 16);
    ASSERT_TRUE(demuxer.add(audio));
    demuxer.complete();
    EXPECT_EQ(0U, listener.added);
    ASSERT_TRUE(audio->inputFrame(makeFrame(CodecAAC, TrackAudio, 0)));
    EXPECT_EQ(1U, listener.added);
    EXPECT_EQ(1U, listener.completed);
    EXPECT_EQ(1U, demuxer.getTracks().size());
}
