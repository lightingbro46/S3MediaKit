#include <gtest/gtest.h>

#include "Http/HttpChunkedSplitter.h"

using namespace mediakit;

static void feed(HttpRequestSplitter &splitter, const std::string &data) {
    std::vector<char> mutable_data(data.begin(), data.end());
    mutable_data.push_back('\0');
    splitter.input(mutable_data.data(), data.size());
}

class CapturingRequestSplitter : public HttpRequestSplitter {
public:
    std::vector<std::string> headers;
    std::vector<std::string> contents;
    ssize_t next_content_length = 0;

protected:
    ssize_t onRecvHeader(const char *data, size_t len) override {
        headers.emplace_back(data, len);
        return next_content_length;
    }
    void onRecvContent(const char *data, size_t len) override {
        contents.emplace_back(data, len);
    }
};

TEST(HttpRequestSplitterTest, CachesPartialHeaderAndParsesCompletion) {
    CapturingRequestSplitter splitter;
    feed(splitter, "GET / HTTP/1.1\r\nHost: ex");
    EXPECT_GT(splitter.remainDataSize(), 0U);
    feed(splitter, "ample.com\r\n\r\n");
    ASSERT_EQ(1U, splitter.headers.size());
    EXPECT_NE(std::string::npos, splitter.headers[0].find("example.com"));
    EXPECT_EQ(0U, splitter.remainDataSize());
}

TEST(HttpRequestSplitterTest, HandlesFixedLengthBodyAndPipelining) {
    CapturingRequestSplitter splitter;
    splitter.next_content_length = 4;
    feed(splitter, "POST /a HTTP/1.1\r\n\r\nbodyPOST /b HTTP/1.1\r\n\r\nnext");
    ASSERT_EQ(2U, splitter.headers.size());
    ASSERT_EQ(2U, splitter.contents.size());
    EXPECT_EQ("body", splitter.contents[0]);
    EXPECT_EQ("next", splitter.contents[1]);
}

TEST(HttpRequestSplitterTest, StreamsVariableLengthContent) {
    CapturingRequestSplitter splitter;
    splitter.next_content_length = -1;
    feed(splitter, "HTTP/1.1 200 OK\r\n\r\nfirst");
    feed(splitter, "second");
    ASSERT_EQ(2U, splitter.contents.size());
    EXPECT_EQ("first", splitter.contents[0]);
    EXPECT_EQ("second", splitter.contents[1]);
    splitter.reset();
    EXPECT_EQ(0U, splitter.remainDataSize());
}

TEST(HttpRequestSplitterTest, EnforcesMaximumCacheSize) {
    CapturingRequestSplitter splitter;
    splitter.setMaxCacheSize(4);
    feed(splitter, "12345");
    EXPECT_THROW(feed(splitter, "6"), std::out_of_range);
    EXPECT_EQ(0U, splitter.remainDataSize());
}

TEST(HttpChunkedSplitterTest, DecodesChunksAcrossInputs) {
    std::vector<std::string> chunks;
    HttpChunkedSplitter splitter([&](const char *data, size_t len) {
        chunks.emplace_back(data ? std::string(data, len) : std::string());
    });
    feed(splitter, "4\r\nWiki\r\n5\r\npe");
    feed(splitter, "dia\r\n0\r\n\r\n");
    ASSERT_EQ(3U, chunks.size());
    EXPECT_EQ("Wiki", chunks[0]);
    EXPECT_EQ("pedia", chunks[1]);
    EXPECT_EQ("", chunks[2]);
}
