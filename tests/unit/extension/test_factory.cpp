#include <gtest/gtest.h>

#include "Extension/Factory.h"

using namespace mediakit;

TEST(FactoryTest, CreatesRegisteredTracksAndFrames) {
    auto h264 = Factory::getTrackByCodecId(CodecH264);
    ASSERT_TRUE(h264);
    EXPECT_EQ(CodecH264, h264->getCodecId());

    auto aac = Factory::getTrackByCodecId(CodecAAC, 48000, 2, 16);
    ASSERT_TRUE(aac);
    EXPECT_EQ(CodecAAC, aac->getCodecId());

    const char payload[] = {0x01, 0x02, 0x03};
    auto frame = Factory::getFrameFromPtr(CodecH264, payload, sizeof(payload), 10, 12);
    ASSERT_TRUE(frame);
    EXPECT_EQ(sizeof(payload), frame->size());
    EXPECT_EQ(10U, frame->dts());
    EXPECT_EQ(12U, frame->pts());
}

TEST(FactoryTest, MapsRtmpMetadataAndRejectsUnknownCodecs) {
    EXPECT_TRUE(Factory::getVideoTrackByAmf(AMFValue(7.0)));
    EXPECT_TRUE(Factory::getAudioTrackByAmf(AMFValue(10.0), 44100, 2, 16));
    EXPECT_FALSE(Factory::getTrackByCodecId(CodecInvalid));
    // Unknown codecs deliberately fall back to the generic RTP packetizer.
    EXPECT_TRUE(Factory::getRtpEncoderByCodecId(CodecInvalid, 96));
    EXPECT_TRUE(Factory::getRtpDecoderByCodecId(CodecInvalid));
    EXPECT_EQ(AMFType::AMF_NULL, Factory::getAmfByCodecId(CodecInvalid).type());
}

TEST(FactoryTest, CreatesCodecSpecificRtpAndRtmpHandlers) {
    auto h264 = Factory::getTrackByCodecId(CodecH264);
    ASSERT_TRUE(h264);
    EXPECT_TRUE(Factory::getRtpEncoderByCodecId(CodecH264, 96));
    EXPECT_TRUE(Factory::getRtpDecoderByCodecId(CodecH264));
    EXPECT_TRUE(Factory::getRtmpEncoderByTrack(h264));
    EXPECT_TRUE(Factory::getRtmpDecoderByTrack(h264));
    EXPECT_NE(AMFType::AMF_NULL, Factory::getAmfByCodecId(CodecH264).type());
}
