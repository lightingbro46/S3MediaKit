#include <gtest/gtest.h>

#include "ext-codec/H264.h"
#include "ext-codec/H265.h"

using namespace mediakit;

TEST(H264HelpersTest, DetectsAnnexBPrefixes) {
    EXPECT_EQ(4U, prefixSize("\x00\x00\x00\x01\x67", 5));
    EXPECT_EQ(3U, prefixSize("\x00\x00\x01\x67", 4));
    EXPECT_EQ(0U, prefixSize("\x01\x02\x03\x04", 4));
    EXPECT_EQ(0U, prefixSize("\x00\x00\x01", 3));
}

TEST(H264HelpersTest, SplitsMixedThreeAndFourBytePrefixes) {
    const std::string annexb =
        std::string("\x00\x00\x00\x01\x67\x11", 6) +
        std::string("\x00\x00\x01\x68\x22", 5) +
        std::string("\x00\x00\x00\x01\x65\x80\x33", 7);
    std::vector<std::string> nalus;
    std::vector<size_t> prefixes;
    splitH264(annexb.data(), annexb.size(), 4,
              [&](const char *data, size_t size, size_t prefix) {
                  nalus.emplace_back(data, size);
                  prefixes.push_back(prefix);
              });
    ASSERT_EQ(3U, nalus.size());
    EXPECT_EQ((std::vector<size_t>{4, 3, 4}), prefixes);
    EXPECT_EQ(7, H264_TYPE(nalus[0][prefixes[0]]));
    EXPECT_EQ(8, H264_TYPE(nalus[1][prefixes[1]]));
    EXPECT_EQ(5, H264_TYPE(nalus[2][prefixes[2]]));
}

TEST(H264FrameTest, ClassifiesNalUnits) {
    H264Frame idr;
    idr._prefix_size = 4;
    idr._buffer.assign("\x00\x00\x00\x01\x65\x80", 6);
    EXPECT_TRUE(idr.keyFrame());
    EXPECT_TRUE(idr.decodeAble());
    EXPECT_FALSE(idr.configFrame());

    H264Frame sps;
    sps._prefix_size = 4;
    sps._buffer.assign("\x00\x00\x00\x01\x67\x00", 6);
    EXPECT_TRUE(sps.configFrame());
    EXPECT_FALSE(sps.keyFrame());

    H264Frame sei;
    sei._prefix_size = 4;
    sei._buffer.assign("\x00\x00\x00\x01\x06\x00", 6);
    EXPECT_TRUE(sei.dropAble());
}

TEST(H265FrameTest, ClassifiesNalUnits) {
    H265Frame idr;
    idr._prefix_size = 4;
    idr._buffer.assign("\x00\x00\x00\x01\x26\x01\x80", 7);
    EXPECT_TRUE(idr.keyFrame());
    EXPECT_TRUE(idr.decodeAble());

    H265Frame vps;
    vps._prefix_size = 4;
    vps._buffer.assign("\x00\x00\x00\x01\x40\x01\x00", 7);
    EXPECT_TRUE(vps.configFrame());

    H265Frame sei;
    sei._prefix_size = 4;
    sei._buffer.assign("\x00\x00\x00\x01\x4e\x01\x00", 7);
    EXPECT_TRUE(sei.dropAble());
}

TEST(H264TrackTest, CollectsConfigurationFrames) {
    H264Track track;
    EXPECT_FALSE(track.ready());
    EXPECT_TRUE(track.getConfigFrames().empty());
    EXPECT_EQ(CodecH264, track.getCodecId());

    Frame::Ptr sps = createConfigFrame<H264Frame>(std::string("\x67\x42\x00\x1e", 4), 0, 0);
    Frame::Ptr pps = createConfigFrame<H264Frame>(std::string("\x68\xce\x06\xe2", 4), 0, 0);
    track.inputFrame(sps);
    track.inputFrame(pps);
    EXPECT_TRUE(track.ready());
    EXPECT_EQ(2U, track.getConfigFrames().size());
}
