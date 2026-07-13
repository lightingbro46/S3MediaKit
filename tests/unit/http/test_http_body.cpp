#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

#include "Http/HttpBody.h"
#include "Network/Buffer.h"

using namespace mediakit;
using namespace toolkit;

static std::string bufferText(const Buffer::Ptr &buffer) {
    return buffer ? std::string(buffer->data(), buffer->size()) : std::string();
}

TEST(HttpStringBodyTest, ReadsRequestedChunksUntilExhausted) {
    HttpStringBody body("abcdefgh");
    EXPECT_EQ(8, body.remainSize());
    EXPECT_EQ("abc", bufferText(body.readData(3)));
    EXPECT_EQ(5, body.remainSize());
    EXPECT_EQ("defgh", bufferText(body.readData(100)));
    EXPECT_EQ(0, body.remainSize());
    EXPECT_EQ(nullptr, body.readData(1));
}

TEST(HttpStringBodyTest, SupportsDefaultAsyncRead) {
    std::shared_ptr<HttpStringBody> body = std::make_shared<HttpStringBody>("value");
    std::string result;
    body->readDataAsync(5, [&](const Buffer::Ptr &buffer) { result = bufferText(buffer); });
    EXPECT_EQ("value", result);
}

TEST(HttpBufferBodyTest, TransfersBufferOnce) {
    Buffer::Ptr source = std::make_shared<BufferString>("payload");
    HttpBufferBody body(source);
    EXPECT_EQ(7, body.remainSize());
    EXPECT_EQ("payload", bufferText(body.readData(2)));
    EXPECT_EQ(0, body.remainSize());
    EXPECT_EQ(nullptr, body.readData(2));
}

TEST(HttpFileBodyTest, ReadsMmapAndStreamRanges) {
    const std::string path = "/tmp/s3mediakit_http_body_test.txt";
    {
        std::ofstream file(path.c_str(), std::ios::binary);
        file << "0123456789";
    }

    HttpFileBody mmap_body(path, true);
    EXPECT_EQ(10, mmap_body.remainSize());
    mmap_body.setRange(2, 5);
    EXPECT_EQ("234", bufferText(mmap_body.readData(3)));
    EXPECT_EQ("56", bufferText(mmap_body.readData(10)));
    EXPECT_EQ(nullptr, mmap_body.readData(1));

    HttpFileBody stream_body(path, false);
    stream_body.setRange(5, 3);
    EXPECT_EQ("567", bufferText(stream_body.readData(10)));

    std::remove(path.c_str());
}

TEST(HttpFileBodyTest, ReportsMissingFile) {
    HttpFileBody body("/tmp/s3mediakit-file-that-does-not-exist", true);
    EXPECT_EQ(-1, body.remainSize());
}

TEST(HttpMultiFormBodyTest, CreatesProtocolFragments) {
    EXPECT_EQ("multipart/form-data; boundary=boundary",
              HttpMultiFormBody::multiFormContentType("boundary"));
    EXPECT_EQ("\r\n--boundary--", HttpMultiFormBody::multiFormBodySuffix("boundary"));
}
