#include <gtest/gtest.h>

#include "Rtcp/RtcpFCI.h"

using namespace mediakit;

TEST(RtcpFciTest, SliRoundTripsBitFields) {
    FCI_SLI sli(0x3FFF, 0x2345, 0x7F);
    EXPECT_EQ(0x1FFF, sli.getFirst());
    EXPECT_EQ(0x0345, sli.getNumber());
    EXPECT_EQ(0x3F, sli.getPicID());
    EXPECT_NE(std::string::npos, sli.dumpString().find("PictureID:63"));
    EXPECT_NO_THROW(sli.check(FCI_SLI::kSize));
}

TEST(RtcpFciTest, FirRoundTripsFields) {
    FCI_FIR fir(0x12345678, 42, 0xABCDEF);
    EXPECT_EQ(0x12345678U, fir.getSSRC());
    EXPECT_EQ(42, fir.getSeq());
    EXPECT_EQ(0xABCDEFU, fir.getReserved());
    EXPECT_NE(std::string::npos, fir.dumpString().find("seq_number:42"));
}

TEST(RtcpFciTest, RembRoundTripsBitrateAndSources) {
    const std::vector<uint32_t> sources = {0x01020304, 0xAABBCCDD};
    std::string packet = FCI_REMB::create(sources, 1000000);
    FCI_REMB *remb = reinterpret_cast<FCI_REMB *>(&packet[0]);
    EXPECT_NO_THROW(remb->check(packet.size()));
    EXPECT_EQ(sources, remb->getSSRC());
    EXPECT_LE(remb->getBitRate(), 1000000U);
    EXPECT_GT(remb->getBitRate(), 990000U);
    EXPECT_NE(std::string::npos, remb->dumpString().find("bitrate:"));
}

TEST(RtcpFciTest, NackBuildsExpectedLossMask) {
    std::vector<bool> losses(16, false);
    losses[0] = true;
    losses[3] = true;
    losses[15] = true;
    FCI_NACK nack(65530, losses);
    EXPECT_EQ(65530, nack.getPid());
    EXPECT_EQ(static_cast<uint16_t>((1U << 0) | (1U << 3) | (1U << 15)), nack.getBlp());
    const std::vector<bool> bits = nack.getBitArray();
    ASSERT_EQ(17U, bits.size());
    EXPECT_TRUE(bits[0]);
    EXPECT_TRUE(bits[1]);
    EXPECT_TRUE(bits[4]);
    EXPECT_TRUE(bits[16]);
}
