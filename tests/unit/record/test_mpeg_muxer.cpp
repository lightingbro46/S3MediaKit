#include <gtest/gtest.h>

#include "Record/MPEG.h"

using namespace mediakit;

namespace {

class CapturingMpegMuxer : public MpegMuxer {
public:
    explicit CapturingMpegMuxer(bool ps = false) : MpegMuxer(ps) {}

    std::vector<toolkit::Buffer::Ptr> buffers;
    std::vector<uint64_t> timestamps;
    std::vector<bool> key_positions;
    size_t discontinuities = 0;

protected:
    void onWrite(toolkit::Buffer::Ptr buffer, uint64_t timestamp, bool key_pos) override {
        if (!buffer) {
            ++discontinuities;
            return;
        }
        buffers.emplace_back(std::move(buffer));
        timestamps.emplace_back(timestamp);
        key_positions.emplace_back(key_pos);
    }
};

Frame::Ptr makeAacFrame(uint64_t dts) {
    static char adts[] = {
        static_cast<char>(0xFF), static_cast<char>(0xF1), 0x50, static_cast<char>(0x80),
        0x01, static_cast<char>(0x7F), static_cast<char>(0xFC), 0x11, 0x22, 0x33
    };
    auto frame = std::make_shared<FrameFromPtr>(CodecAAC, adts, sizeof(adts), dts, dts, 7);
    frame->setIndex(TrackAudio);
    return frame;
}

Frame::Ptr makeVideoFrame(CodecId codec, uint64_t dts, bool key) {
    static char h264[] = {0, 0, 0, 1, 0x65, 0x01, 0x02, 0x03};
    auto frame = std::make_shared<FrameFromPtr>(codec, h264, sizeof(h264), dts, dts, 4, key);
    frame->setIndex(TrackVideo);
    return frame;
}

} // namespace

TEST(MpegMuxerTest, MuxesAudioOnlyIntoTransportStream) {
    CapturingMpegMuxer muxer;
    auto audio = std::make_shared<AudioTrackImp>(CodecAAC, 48000, 2, 16);
    ASSERT_TRUE(muxer.addTrack(audio));
    EXPECT_FALSE(muxer.inputFrame(makeVideoFrame(CodecH264, 0, true)));
    EXPECT_TRUE(muxer.inputFrame(makeAacFrame(100)));
    ASSERT_FALSE(muxer.buffers.empty());
    EXPECT_EQ(100U, muxer.timestamps.back());
    // The first emitted TS batch is the random-access position; later batches
    // from the same mux operation clear the marker.
    EXPECT_TRUE(muxer.key_positions.front());
    EXPECT_GT(muxer.buffers.back()->size(), 0U);
    EXPECT_EQ(0x47, static_cast<uint8_t>(muxer.buffers.back()->data()[0]));
}

TEST(MpegMuxerTest, MuxesVideoAndFlushesFrameMerger) {
    CapturingMpegMuxer muxer;
    auto video = std::make_shared<VideoTrackImp>(CodecH264, 1280, 720, 25);
    ASSERT_TRUE(muxer.addTrack(video));
    EXPECT_TRUE(muxer.inputFrame(makeVideoFrame(CodecH264, 0, true)));
    EXPECT_TRUE(muxer.inputFrame(makeVideoFrame(CodecH264, 40, false)));
    muxer.flush();
    ASSERT_FALSE(muxer.buffers.empty());
    EXPECT_TRUE(muxer.key_positions.front());
}

TEST(MpegMuxerTest, ResetsContextAndRejectsUnsupportedTrack) {
    CapturingMpegMuxer muxer(true);
    auto unsupported = std::make_shared<VideoTrackImp>(CodecInvalid, 320, 240, 15);
    EXPECT_FALSE(muxer.addTrack(unsupported));
    auto audio = std::make_shared<AudioTrackImp>(CodecAAC, 8000, 1, 16);
    EXPECT_TRUE(muxer.addTrack(audio));
    EXPECT_TRUE(muxer.inputFrame(makeAacFrame(20)));
    muxer.resetTracks();
    EXPECT_EQ(1U, muxer.discontinuities);
    EXPECT_FALSE(muxer.inputFrame(makeAacFrame(40)));
    EXPECT_TRUE(muxer.addTrack(audio));
    EXPECT_TRUE(muxer.inputFrame(makeAacFrame(60)));
}

TEST(MpegMuxerTest, RequiresAdtsPrefixForAac) {
    CapturingMpegMuxer muxer;
    auto audio = std::make_shared<AudioTrackImp>(CodecAAC, 48000, 2, 16);
    ASSERT_TRUE(muxer.addTrack(audio));
    static char raw[] = {1, 2, 3};
    auto frame = std::make_shared<FrameFromPtr>(CodecAAC, raw, sizeof(raw), 0, 0, 0);
    frame->setIndex(TrackAudio);
    EXPECT_THROW(muxer.inputFrame(frame), std::runtime_error);
}
