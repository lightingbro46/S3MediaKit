#include <gtest/gtest.h>

#include "Record/HlsMaker.h"

using namespace mediakit;

namespace {

class CapturingHlsMaker : public HlsMaker {
public:
    CapturingHlsMaker(bool fmp4, float duration, uint32_t count, bool keep)
        : HlsMaker(fmp4, duration, count, keep) {}

    std::vector<uint64_t> opened;
    std::vector<uint64_t> deleted;
    std::vector<std::string> segment_data;
    std::vector<std::string> init_data;
    std::vector<std::pair<std::string, bool> > playlists;
    std::vector<uint64_t> flushed_duration;

    void finish(bool eof) { flushLastSegment(eof); }

protected:
    std::string onOpenSegment(uint64_t index) override {
        opened.emplace_back(index);
        return std::to_string(index) + (isFmp4() ? ".m4s" : ".ts");
    }
    void onDelSegment(uint64_t index) override { deleted.emplace_back(index); }
    void onWriteInitSegment(const char *data, size_t len) override {
        init_data.emplace_back(data, len);
    }
    void onWriteSegment(const char *data, size_t len) override {
        segment_data.emplace_back(data, len);
    }
    void onWriteHls(const std::string &data, bool include_delay) override {
        playlists.emplace_back(data, include_delay);
    }
    void onFlushLastSegment(uint64_t duration) override {
        flushed_duration.emplace_back(duration);
    }
};

} // namespace

TEST(HlsMakerTest, CreatesLiveTsSegmentsAndPlaylist) {
    CapturingHlsMaker maker(false, 1.0F, 3, false);
    EXPECT_TRUE(maker.isLive());
    EXPECT_FALSE(maker.isKeep());
    EXPECT_FALSE(maker.isFmp4());

    maker.inputData("a", 1, 0, true);
    maker.inputData("b", 1, 500, false);
    maker.inputData("c", 1, 1100, true);
    maker.inputData("d", 1, 1600, false);
    maker.inputData("e", 1, 2200, true);
    maker.inputData("f", 1, 2400, false);
    maker.finish(true);

    ASSERT_EQ(3U, maker.opened.size());
    EXPECT_EQ(6U, maker.segment_data.size());
    ASSERT_FALSE(maker.playlists.empty());
    const auto &playlist = maker.playlists.back().first;
    EXPECT_NE(std::string::npos, playlist.find("#EXTM3U"));
    EXPECT_NE(std::string::npos, playlist.find("#EXT-X-VERSION:4"));
    EXPECT_NE(std::string::npos, playlist.find("0.ts"));
    EXPECT_NE(std::string::npos, playlist.find("2.ts"));
    EXPECT_NE(std::string::npos, playlist.find("#EXT-X-ENDLIST"));
    EXPECT_EQ(3U, maker.flushed_duration.size());
}

TEST(HlsMakerTest, SupportsFmp4InitAndEventPlaylist) {
    CapturingHlsMaker maker(true, 0.5F, 0, true);
    EXPECT_FALSE(maker.isLive());
    EXPECT_TRUE(maker.isKeep());
    EXPECT_TRUE(maker.isFmp4());
    maker.inputInitSegment("init", 4);
    ASSERT_EQ(1U, maker.init_data.size());
    EXPECT_EQ("init", maker.init_data[0]);

    maker.inputData("one", 3, 100, true);
    maker.inputData("two", 3, 700, true);
    maker.finish(true);
    ASSERT_FALSE(maker.playlists.empty());
    const auto &playlist = maker.playlists.back().first;
    EXPECT_NE(std::string::npos, playlist.find("#EXT-X-VERSION:7"));
    EXPECT_NE(std::string::npos, playlist.find("#EXT-X-PLAYLIST-TYPE:EVENT"));
    EXPECT_NE(std::string::npos, playlist.find("#EXT-X-MAP:URI=\"init.mp4\""));
    EXPECT_TRUE(maker.deleted.empty());
}

TEST(HlsMakerTest, RejectsInitForTsAndHandlesResetAndTimestampRollback) {
    CapturingHlsMaker maker(false, 0.1F, 2, false);
    EXPECT_THROW(maker.inputInitSegment("x", 1), std::invalid_argument);
    maker.inputData(nullptr, 0, 0, false);
    EXPECT_TRUE(maker.playlists.empty());

    maker.inputData("a", 1, 1000, true);
    maker.inputData("b", 1, 1200, false);
    maker.inputData("c", 1, 100, false);
    maker.inputData(nullptr, 0, 0, false);
    EXPECT_FALSE(maker.playlists.empty());
    maker.clear();
    maker.inputData("d", 1, 0, true);
    EXPECT_EQ(2U, maker.opened.size());
}

TEST(HlsMakerTest, DeletesOldSegmentsWhenRetentionWindowIsExceeded) {
    CapturingHlsMaker maker(false, 0.01F, 1, false);
    for (uint64_t i = 0; i < 12; ++i) {
        maker.inputData("x", 1, i * 20, true);
    }
    maker.finish(false);
    EXPECT_FALSE(maker.deleted.empty());
    EXPECT_LT(maker.deleted.front(), maker.opened.back());
}
