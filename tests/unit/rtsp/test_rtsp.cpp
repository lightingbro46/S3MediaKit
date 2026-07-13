#include <gtest/gtest.h>

#include "Rtsp/Rtsp.h"

using namespace mediakit;

TEST(RtpPayloadTest, ReturnsStaticPayloadMetadata) {
    EXPECT_EQ(8000, RtpPayload::getClockRate(Rtsp::PT_PCMU));
    EXPECT_EQ(TrackAudio, RtpPayload::getTrackType(Rtsp::PT_PCMA));
    EXPECT_EQ(1, RtpPayload::getAudioChannel(Rtsp::PT_PCMU));
    EXPECT_STREQ("PCMU", RtpPayload::getName(Rtsp::PT_PCMU));
    EXPECT_EQ(CodecG711U, RtpPayload::getCodecId(Rtsp::PT_PCMU));
    EXPECT_EQ(90000, RtpPayload::getClockRateByCodec(CodecH264));
    EXPECT_EQ(90000, RtpPayload::getClockRate(127));
}

TEST(SdpParserTest, ParsesAudioAndVideoTracks) {
    const std::string sdp =
        "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=test\r\nt=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\na=control:trackID=0\r\n"
        "m=audio 0 RTP/AVP 8\r\na=rtpmap:8 PCMA/8000/1\r\na=control:trackID=1\r\n";
    SdpParser parser(sdp);
    EXPECT_TRUE(parser.available());
    ASSERT_TRUE(parser.getTrack(TrackVideo));
    EXPECT_EQ("H264", parser.getTrack(TrackVideo)->_codec);
    EXPECT_EQ(90000, parser.getTrack(TrackVideo)->_samplerate);
    ASSERT_TRUE(parser.getTrack(TrackAudio));
    EXPECT_EQ(CodecG711A, RtpPayload::getCodecId(parser.getTrack(TrackAudio)->_pt));
    EXPECT_EQ(2U, parser.getAvailableTrack().size());
    EXPECT_NE(std::string::npos, parser.toString().find("m=video"));
    EXPECT_EQ("rtsp://example.com/live/trackID=0",
              parser.getTrack(TrackVideo)->getControlUrl("rtsp://example.com/live"));
}

TEST(SdpParserTest, HandlesMissingAndAbsoluteControlUrls) {
    SdpParser empty;
    EXPECT_FALSE(empty.available());
    EXPECT_FALSE(empty.getTrack(TrackVideo));

    SdpTrack track;
    track._control = "rtsp://other.example/track";
    EXPECT_EQ(track._control, track.getControlUrl("rtsp://base/live"));
}

TEST(RtpHeaderTest, ReadsBasicHeaderAndPayload) {
    RtpPacket::Ptr packet = RtpPacket::create();
    packet->setCapacity(RtpPacket::kRtpTcpHeaderSize + RtpPacket::kRtpHeaderSize + 4);
    packet->setSize(RtpPacket::kRtpTcpHeaderSize + RtpPacket::kRtpHeaderSize + 4);
    memset(packet->data(), 0, packet->size());
    RtpHeader *header = packet->getHeader();
    header->version = 2;
    header->pt = 96;
    header->mark = 1;
    header->seq = htons(321);
    header->stamp = htonl(90000);
    header->ssrc = htonl(0x12345678);
    memcpy(packet->getPayload(), "data", 4);
    packet->sample_rate = 90000;
    packet->ntp_stamp = 1000;

    EXPECT_EQ(321, packet->getSeq());
    EXPECT_EQ(90000U, packet->getStamp());
    EXPECT_EQ(0x12345678U, packet->getSSRC());
    EXPECT_EQ(4U, packet->getPayloadSize());
    EXPECT_EQ("data", std::string(reinterpret_cast<char *>(packet->getPayload()), 4));
    EXPECT_FALSE(packet->dumpString().empty());
}
