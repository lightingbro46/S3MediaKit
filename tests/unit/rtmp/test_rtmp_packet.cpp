#include <gtest/gtest.h>

#include "Rtmp/Rtmp.h"
#include "ext-codec/AAC.h"

using namespace mediakit;

TEST(RtmpPacketTest, ClearsPacketState) {
    RtmpPacket::Ptr packet = RtmpPacket::create();
    packet->type_id = MSG_VIDEO;
    packet->time_stamp = 123;
    packet->buffer = "data";
    packet->clear();
    EXPECT_EQ(0U, packet->time_stamp);
    EXPECT_TRUE(packet->buffer.empty());
    EXPECT_EQ(0U, packet->size());
}

TEST(RtmpPacketTest, DetectsClassicVideoFrames) {
    RtmpPacket::Ptr packet = RtmpPacket::create();
    packet->type_id = MSG_VIDEO;
    packet->buffer.assign("\x17\x00\x00\x00\x00", 5);
    EXPECT_TRUE(packet->isVideoKeyFrame());
    EXPECT_TRUE(packet->isConfigFrame());
    EXPECT_EQ(static_cast<int>(RtmpVideoCodec::h264), packet->getRtmpCodecId());

    packet->buffer.assign("\x27\x01\x00\x00\x00", 5);
    EXPECT_FALSE(packet->isVideoKeyFrame());
    EXPECT_FALSE(packet->isConfigFrame());
}

TEST(RtmpPacketTest, ReadsAudioFlags) {
    RtmpPacket::Ptr packet = RtmpPacket::create();
    packet->type_id = MSG_AUDIO;
    packet->buffer.assign("\xAF\x00", 2);
    EXPECT_TRUE(packet->isConfigFrame());
    EXPECT_EQ(44100, packet->getAudioSampleRate());
    EXPECT_EQ(16, packet->getAudioSampleBit());
    EXPECT_EQ(2, packet->getAudioChannel());
}

TEST(RtmpMetadataTest, CreatesTitleAndAudioMetadata) {
    TitleMeta title(12.5F, 1024, {{"author", "test"}});
    EXPECT_DOUBLE_EQ(12.5, title.getMetadata()["duration"].as_number());
    EXPECT_EQ("test", title.getMetadata()["author"].as_string());

    std::shared_ptr<AACTrack> track = std::make_shared<AACTrack>(std::string("\x12\x10", 2));
    AudioMeta audio(track);
    EXPECT_EQ(static_cast<int>(RtmpAudioCodec::aac), audio.getMetadata()["audiocodecid"].as_integer());
    EXPECT_EQ(44100, audio.getMetadata()["audiosamplerate"].as_integer());
    EXPECT_NE(0, getAudioRtmpFlags(track));
}
