#include <gtest/gtest.h>

#include "Rtmp/FlvSplitter.h"

using namespace mediakit;

namespace {

class CapturingFlvSplitter : public FlvSplitter {
public:
    size_t header_count = 0;
    size_t metadata_count = 0;
    std::vector<RtmpPacket::Ptr> packets;
    bool accept_metadata = true;

protected:
    void onRecvFlvHeader(const FLVHeader &) override { ++header_count; }
    bool onRecvMetadata(const AMFValue &) override {
        ++metadata_count;
        return accept_metadata;
    }
    void onRecvRtmpPacket(RtmpPacket::Ptr packet) override {
        packets.emplace_back(std::move(packet));
    }
};

void appendBe24(std::string &out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendBe32(std::string &out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

std::string flvHeader(uint8_t flags = 0x05) {
    std::string out("FLV", 3);
    out.push_back(1);
    out.push_back(static_cast<char>(flags));
    appendBe32(out, 9);
    appendBe32(out, 0);
    return out;
}

std::string flvTag(uint8_t type, uint32_t timestamp, const std::string &payload,
                   uint32_t previous_size_adjustment = 0) {
    std::string out;
    out.push_back(static_cast<char>(type));
    appendBe24(out, payload.size());
    appendBe24(out, timestamp & 0xFFFFFF);
    out.push_back(static_cast<char>((timestamp >> 24) & 0xFF));
    appendBe24(out, 0);
    out += payload;
    appendBe32(out, 11 + payload.size() + previous_size_adjustment);
    return out;
}

void feed(FlvSplitter &splitter, const std::string &bytes) {
    std::vector<char> mutable_bytes(bytes.begin(), bytes.end());
    mutable_bytes.push_back('\0');
    splitter.input(mutable_bytes.data(), bytes.size());
}

} // namespace

TEST(FlvSplitterTest, ParsesHeaderAndAudioVideoTags) {
    CapturingFlvSplitter splitter;
    std::string bytes = flvHeader();
    bytes += flvTag(MSG_AUDIO, 0x01020304, std::string("\xAF\x01", 2));
    bytes += flvTag(MSG_VIDEO, 25, std::string("\x17\x01\0", 3), 1);
    feed(splitter, bytes);

    EXPECT_EQ(1U, splitter.header_count);
    ASSERT_EQ(2U, splitter.packets.size());
    EXPECT_EQ(MSG_AUDIO, splitter.packets[0]->type_id);
    EXPECT_EQ(CHUNK_AUDIO, splitter.packets[0]->chunk_id);
    EXPECT_EQ(0x01020304U, splitter.packets[0]->time_stamp);
    EXPECT_EQ(2U, splitter.packets[0]->body_size);
    EXPECT_EQ(MSG_VIDEO, splitter.packets[1]->type_id);
    EXPECT_EQ(CHUNK_VIDEO, splitter.packets[1]->chunk_id);
}

TEST(FlvSplitterTest, RejectsInvalidHeaders) {
    CapturingFlvSplitter splitter;
    EXPECT_THROW(feed(splitter, "NOT A FLV HEADER"), std::invalid_argument);

    std::string wrong_version = flvHeader();
    wrong_version[3] = 2;
    CapturingFlvSplitter version_splitter;
    EXPECT_THROW(feed(version_splitter, wrong_version), std::invalid_argument);

    CapturingFlvSplitter no_streams_splitter;
    EXPECT_THROW(feed(no_streams_splitter, flvHeader(0)), std::invalid_argument);
}

TEST(FlvSplitterTest, IgnoresUnknownTagTypes) {
    CapturingFlvSplitter splitter;
    feed(splitter, flvHeader() + flvTag(MSG_ACK, 0, "x"));
    EXPECT_EQ(1U, splitter.header_count);
    EXPECT_TRUE(splitter.packets.empty());
}
