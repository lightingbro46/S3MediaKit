#include <gtest/gtest.h>

#include "Rtcp/Rtcp.h"
#include "Rtcp/RtcpFCI.h"

using namespace mediakit;

TEST(RtcpTypesTest, ReturnsDescriptionsForKnownAndUnknownValues) {
    EXPECT_NE(std::string::npos, std::string(rtcpTypeToStr(RtcpType::RTCP_SR)).find("RTCP_SR"));
    EXPECT_NE(std::string::npos, std::string(sdesTypeToStr(SdesType::RTCP_SDES_CNAME)).find("CNAME"));
    EXPECT_NE(std::string::npos, std::string(psfbTypeToStr(PSFBType::RTCP_PSFB_PLI)).find("PLI"));
    EXPECT_NE(std::string::npos, std::string(rtpfbTypeToStr(RTPFBType::RTCP_RTPFB_NACK)).find("NACK"));
    EXPECT_NE(nullptr, rtcpTypeToStr(static_cast<RtcpType>(255)));
}

TEST(RtcpHeaderTest, SetsSizeAndSerializesRoundTrip) {
    std::shared_ptr<RtcpSR> sr = RtcpSR::create(0);
    sr->ssrc = htonl(0x12345678);
    sr->rtpts = htonl(90000);
    sr->packet_count = htonl(10);
    sr->octet_count = htonl(1000);
    sr->setNtpStamp(1700000000000ULL);
    toolkit::Buffer::Ptr buffer = RtcpHeader::toBuffer(sr);
    ASSERT_TRUE(buffer);
    std::string bytes(buffer->data(), buffer->size());
    std::vector<RtcpHeader *> packets = RtcpHeader::loadFromBytes(&bytes[0], bytes.size());
    ASSERT_EQ(1U, packets.size());
    EXPECT_EQ(RtcpType::RTCP_SR, static_cast<RtcpType>(packets[0]->pt));
    EXPECT_EQ(bytes.size(), packets[0]->getSize());
    EXPECT_NE(std::string::npos, packets[0]->dumpString().find("RTCP_SR"));
}

TEST(RtcpPacketTest, CreatesFeedbackSdesAndByePackets) {
    FCI_NACK nack(100, std::vector<bool>{true, false, true});
    std::shared_ptr<RtcpFB> fb = RtcpFB::create(RTPFBType::RTCP_RTPFB_NACK, &nack, FCI_NACK::kSize);
    EXPECT_EQ(RtcpType::RTCP_RTPFB, static_cast<RtcpType>(fb->pt));
    EXPECT_GT(fb->getSize(), sizeof(RtcpHeader));

    std::shared_ptr<RtcpSdes> sdes = RtcpSdes::create({"camera", "server"});
    EXPECT_EQ(RtcpType::RTCP_SDES, static_cast<RtcpType>(sdes->pt));
    EXPECT_GT(sdes->getSize(), sizeof(RtcpHeader));

    std::shared_ptr<RtcpBye> bye = RtcpBye::create({1, 2}, "shutdown");
    EXPECT_EQ(RtcpType::RTCP_BYE, static_cast<RtcpType>(bye->pt));
    EXPECT_GT(bye->getSize(), sizeof(RtcpHeader));
}

TEST(RtcpHeaderTest, RejectsTruncatedAndInvalidPackets) {
    char tiny[3] = {0};
    EXPECT_TRUE(RtcpHeader::loadFromBytes(tiny, sizeof(tiny)).empty());
    char invalid[8] = {0};
    invalid[0] = 0;
    EXPECT_TRUE(RtcpHeader::loadFromBytes(invalid, sizeof(invalid)).empty());
}
