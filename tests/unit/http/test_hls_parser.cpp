#include <gtest/gtest.h>

#include "Http/HlsParser.h"

using namespace mediakit;

class CapturingHlsParser : public HlsParser {
public:
    bool accept = true;
    bool inner = false;
    int64_t sequence = -1;
    std::map<int, ts_segment> segments;

protected:
    bool onParsed(bool value, int64_t seq, const std::map<int, ts_segment> &items) override {
        inner = value;
        sequence = seq;
        segments = items;
        return accept;
    }
};

TEST(HlsParserTest, ParsesMediaPlaylist) {
    const std::string playlist =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-ALLOW-CACHE:YES\n"
        "#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:7\n"
        "#EXTINF:4.5,\nsegment7.ts\n#EXTINF:5.25,\nsegment8.ts\n#EXT-X-ENDLIST\n";
    CapturingHlsParser parser;
    ASSERT_TRUE(parser.parse("https://example.com/live/index.m3u8", playlist));
    EXPECT_TRUE(parser.isM3u8());
    EXPECT_TRUE(parser.allowCache());
    EXPECT_FALSE(parser.isLive());
    EXPECT_EQ(3, parser.getVersion());
    EXPECT_EQ(10, parser.getTargetDur());
    EXPECT_EQ(7, parser.getSequence());
    EXPECT_FALSE(parser.isM3u8Inner());
    EXPECT_FLOAT_EQ(9.75F, parser.getTotalDuration());
    ASSERT_EQ(2U, parser.segments.size());
    EXPECT_EQ("https://example.com/live/segment7.ts", parser.segments[0].url);
}

TEST(HlsParserTest, ParsesMasterPlaylistByBandwidth) {
    const std::string playlist =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=800000,RESOLUTION=640x360\nlow.m3u8\n"
        "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=2400000,RESOLUTION=1920x1080\nhigh.m3u8\n";
    CapturingHlsParser parser;
    ASSERT_TRUE(parser.parse("http://example.com/master.m3u8", playlist));
    EXPECT_TRUE(parser.inner);
    ASSERT_EQ(2U, parser.segments.size());
    EXPECT_EQ(640, parser.segments[800000].width);
    EXPECT_EQ(1080, parser.segments[2400000].height);
}

TEST(HlsParserTest, RejectsInvalidOrCallbackRejectedPlaylist) {
    CapturingHlsParser parser;
    EXPECT_FALSE(parser.parse("http://example.com/a.m3u8", "not a playlist"));
    parser.accept = false;
    EXPECT_FALSE(parser.parse("http://example.com/a.m3u8", "#EXTM3U\n#EXTINF:1,\na.ts\n"));
}
