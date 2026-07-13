#include <gtest/gtest.h>

#include "Rtsp/RtpReceiver.h"

using namespace mediakit;

namespace {

std::vector<uint8_t> makeRtp(uint16_t seq, uint32_t stamp, uint32_t ssrc,
                             uint8_t pt = 96, const std::string &payload = "data") {
    std::vector<uint8_t> out(12 + payload.size());
    out[0] = 0x80;
    out[1] = pt & 0x7F;
    out[2] = static_cast<uint8_t>(seq >> 8);
    out[3] = static_cast<uint8_t>(seq);
    out[4] = static_cast<uint8_t>(stamp >> 24);
    out[5] = static_cast<uint8_t>(stamp >> 16);
    out[6] = static_cast<uint8_t>(stamp >> 8);
    out[7] = static_cast<uint8_t>(stamp);
    out[8] = static_cast<uint8_t>(ssrc >> 24);
    out[9] = static_cast<uint8_t>(ssrc >> 16);
    out[10] = static_cast<uint8_t>(ssrc >> 8);
    out[11] = static_cast<uint8_t>(ssrc);
    std::copy(payload.begin(), payload.end(), out.begin() + 12);
    return out;
}

} // namespace

TEST(PacketSortorTest, ReordersPacketsAndFlushesContiguousRange) {
    PacketSortor<std::string> sorter;
    std::vector<uint16_t> sequences;
    std::vector<std::string> packets;
    sorter.setOnSort([&](uint16_t seq, std::string packet) {
        sequences.emplace_back(seq);
        packets.emplace_back(std::move(packet));
    });

    sorter.sortPacket(10, "ten");
    sorter.sortPacket(12, "twelve");
    EXPECT_EQ(1U, sorter.getJitterSize());
    sorter.sortPacket(11, "eleven");
    EXPECT_EQ(0U, sorter.getJitterSize());
    EXPECT_EQ((std::vector<uint16_t>{10, 11, 12}), sequences);
    EXPECT_EQ((std::vector<std::string>{"ten", "eleven", "twelve"}), packets);
}

TEST(PacketSortorTest, ForceFlushesOnDistanceAndExplicitFlush) {
    PacketSortor<int> sorter;
    std::vector<uint16_t> sequences;
    sorter.setOnSort([&](uint16_t seq, int) { sequences.emplace_back(seq); });
    sorter.setParams(2, 10000, 3);
    sorter.sortPacket(100, 1);
    sorter.sortPacket(102, 2);
    sorter.sortPacket(110, 3);
    EXPECT_EQ((std::vector<uint16_t>{100, 102}), sequences);
    sorter.sortPacket(111, 4);
    sorter.flush();
    // Packet 110 is discarded after the forced jump because it is then too
    // far behind the new expected sequence.
    EXPECT_EQ((std::vector<uint16_t>{100, 102, 111}), sequences);
    sorter.clear();
    EXPECT_EQ(0U, sorter.getJitterSize());
}

TEST(PacketSortorTest, HandlesSequenceNumberWrap) {
    PacketSortor<int> sorter;
    std::vector<uint16_t> sequences;
    sorter.setOnSort([&](uint16_t seq, int) { sequences.emplace_back(seq); });
    sorter.sortPacket(65534, 1);
    sorter.sortPacket(0, 3);
    sorter.sortPacket(65535, 2);
    EXPECT_EQ((std::vector<uint16_t>{65534, 65535, 0}), sequences);
}

TEST(RtpTrackTest, ParsesSortsAndTimestampsPackets) {
    RtpTrackImp track;
    std::vector<uint16_t> before;
    std::vector<uint16_t> sorted;
    track.setBeforeSorted([&](const RtpPacket::Ptr &packet) {
        before.emplace_back(packet->getSeq());
    });
    track.setOnSorted([&](RtpPacket::Ptr packet) {
        sorted.emplace_back(packet->getSeq());
    });
    track.setNtpStamp(90000, 1000);

    auto p1 = makeRtp(1, 90000, 0x11223344);
    auto p3 = makeRtp(3, 180000, 0x11223344);
    auto p2 = makeRtp(2, 135000, 0x11223344);
    auto first = track.inputRtp(TrackVideo, 90000, p1.data(), p1.size());
    ASSERT_TRUE(first);
    EXPECT_EQ(0x11223344U, track.getSSRC());
    EXPECT_EQ(1000U, first->ntp_stamp);
    EXPECT_TRUE(track.inputRtp(TrackVideo, 90000, p3.data(), p3.size()));
    EXPECT_EQ(1U, track.getJitterSize());
    EXPECT_TRUE(track.inputRtp(TrackVideo, 90000, p2.data(), p2.size()));
    EXPECT_EQ((std::vector<uint16_t>{1, 2, 3}), sorted);
    EXPECT_EQ((std::vector<uint16_t>{1, 3, 2}), before);
}

TEST(RtpTrackTest, ValidatesHeaderPayloadTypeRateAndSsrc) {
    RtpTrackImp track;
    auto packet = makeRtp(1, 100, 1, 96);
    EXPECT_THROW(track.inputRtp(TrackVideo, 90000, packet.data(), 5), RtpTrack::BadRtpException);

    auto bad_version = packet;
    bad_version[0] = 0x40;
    EXPECT_THROW(track.inputRtp(TrackVideo, 90000, bad_version.data(), bad_version.size()),
                 RtpTrack::BadRtpException);
    EXPECT_FALSE(track.inputRtp(TrackVideo, 0, packet.data(), packet.size()));

    track.setPayloadType(97);
    EXPECT_FALSE(track.inputRtp(TrackVideo, 90000, packet.data(), packet.size()));
    track.setPayloadType(96);
    EXPECT_TRUE(track.inputRtp(TrackVideo, 90000, packet.data(), packet.size()));

    auto other_ssrc = makeRtp(2, 200, 2, 96);
    EXPECT_FALSE(track.inputRtp(TrackVideo, 90000, other_ssrc.data(), other_ssrc.size()));
    track.clear();
    EXPECT_EQ(0U, track.getSSRC());
}

TEST(RtpTrackTest, CanUseRtpTimestampDirectlyAsNtp) {
    RtpTrackImp track;
    track.setNtpStamp(0, 0);
    auto packet = makeRtp(1, 48000, 5, 97);
    auto parsed = track.inputRtp(TrackAudio, 48000, packet.data(), packet.size());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(1000U, parsed->ntp_stamp);
    EXPECT_EQ(TrackAudio, parsed->type);
    EXPECT_EQ('$', parsed->data()[0]);
    EXPECT_EQ(2 * TrackAudio, static_cast<uint8_t>(parsed->data()[1]));
}
