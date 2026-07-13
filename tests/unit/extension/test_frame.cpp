#include <gtest/gtest.h>

#include "Extension/Frame.h"

using namespace mediakit;

TEST(FrameMappingTest, MapsTrackAndCodecNames) {
    EXPECT_EQ(TrackVideo, getTrackType("video"));
    EXPECT_EQ(TrackAudio, getTrackType("audio"));
    EXPECT_EQ(TrackInvalid, getTrackType("unknown"));
    EXPECT_STREQ("video", getTrackString(TrackVideo));
    EXPECT_STREQ("audio", getTrackString(TrackAudio));
    EXPECT_EQ(CodecH264, getCodecId("H264"));
    EXPECT_EQ(CodecAAC, getCodecId("mpeg4-generic"));
    EXPECT_EQ(CodecInvalid, getCodecId("missing"));
    EXPECT_STREQ("H265", getCodecName(CodecH265));
    EXPECT_EQ(TrackVideo, getTrackType(CodecVP9));
    EXPECT_EQ(TrackAudio, getTrackType(CodecOpus));
}

TEST(FrameMappingTest, RoundTripsMpegAndMovObjectIds) {
    for (int codec = CodecH264; codec < CodecMax; ++codec) {
        const CodecId id = static_cast<CodecId>(codec);
        const int mpeg = getMpegIdByCodec(id);
        const CodecId from_mpeg = getCodecByMpegId(mpeg);
        if (from_mpeg != CodecInvalid) {
            EXPECT_EQ(id, from_mpeg);
        }
        const int mov = getMovIdByCodec(id);
        const CodecId from_mov = getCodecByMovId(mov);
        if (from_mov != CodecInvalid) {
            EXPECT_EQ(id, from_mov);
        }
    }
    EXPECT_EQ(CodecInvalid, getCodecByMpegId(-123));
    EXPECT_EQ(CodecInvalid, getCodecByMovId(-123));
}

TEST(FrameTest, CachesPointerBackedFrame) {
    char bytes[] = "payload";
    Frame::Ptr source = std::make_shared<FrameFromPtr>(CodecAAC, bytes, 7, 100, 120, 0, true);
    EXPECT_FALSE(source->cacheAble());
    EXPECT_EQ(100U, source->dts());
    EXPECT_EQ(120U, source->pts());
    EXPECT_TRUE(source->keyFrame());
    Frame::Ptr cached = Frame::getCacheAbleFrame(source);
    ASSERT_TRUE(cached);
    EXPECT_TRUE(cached->cacheAble());
    EXPECT_EQ("payload", std::string(cached->data(), cached->size()));
    EXPECT_EQ(CodecAAC, cached->getCodecId());
}

TEST(FrameTest, FrameImpUsesDtsAsDefaultPtsAndCustomIndex) {
    FrameImp::Ptr frame = FrameImp::create();
    frame->_codec_id = CodecMP3;
    frame->_dts = 55;
    frame->_buffer = "abc";
    EXPECT_EQ(55U, frame->pts());
    EXPECT_EQ(TrackAudio, frame->getTrackType());
    EXPECT_EQ(TrackAudio, frame->getIndex());
    frame->setIndex(7);
    EXPECT_EQ(7, frame->getIndex());
    EXPECT_TRUE(frame->decodeAble());
}

TEST(FrameTest, InvalidPointerFrameRejectsCodecLookup) {
    char byte = 0;
    FrameFromPtr frame(CodecInvalid, &byte, 1, 0);
    EXPECT_THROW(frame.getCodecId(), std::invalid_argument);
}
