#include <gtest/gtest.h>

#include <cstdio>

#include "Record/MP4.h"

using namespace mediakit;

#if defined(ENABLE_MP4)

namespace {

class TestMemoryFile : public MP4FileMemory {
public:
    uint64_t tell() { return onTell(); }
    int seek(uint64_t offset) { return onSeek(offset); }
    int read(void *data, size_t size) { return onRead(data, size); }
    int write(const void *data, size_t size) { return onWrite(data, size); }
};

class TestDiskFile : public MP4FileDisk {
public:
    uint64_t tell() { return onTell(); }
    int seek(uint64_t offset) { return onSeek(offset); }
    int read(void *data, size_t size) { return onRead(data, size); }
    int write(const void *data, size_t size) { return onWrite(data, size); }
};

} // namespace

TEST(MP4FileMemoryTest, ReadsWritesSeeksAndClearsMemory) {
    auto file = std::make_shared<TestMemoryFile>();
    EXPECT_EQ(0U, file->fileSize());
    EXPECT_EQ(0U, file->tell());
    EXPECT_EQ(0, file->write("abcdef", 6));
    EXPECT_EQ(6U, file->fileSize());
    EXPECT_EQ(6U, file->tell());
    EXPECT_EQ(-1, file->seek(7));
    EXPECT_EQ(0, file->seek(2));
    EXPECT_EQ(0, file->write("XY", 2));

    EXPECT_EQ(0, file->seek(0));
    char buffer[16] = {0};
    EXPECT_EQ(0, file->read(buffer, sizeof(buffer)));
    EXPECT_EQ("abXYef", std::string(buffer, 6));
    EXPECT_EQ(-1, file->read(buffer, 1));

    EXPECT_EQ("abXYef", file->getAndClearMemory());
    EXPECT_EQ(0U, file->fileSize());
    EXPECT_EQ(0U, file->tell());
}

TEST(MP4FileMemoryTest, CreatesAndDestroysMp4Writer) {
    auto file = std::make_shared<TestMemoryFile>();
    auto writer = file->createWriter(0, false);
    ASSERT_TRUE(writer);
    writer.reset();
    EXPECT_GT(file->fileSize(), 0U);
    EXPECT_EQ(file->fileSize(), file->tell());
}

TEST(MP4FileDiskTest, OpensWritesReadsAndClosesFile) {
    const std::string path = "/tmp/s3mediakit-unit-mp4-io.bin";
    std::remove(path.c_str());
    auto file = std::make_shared<TestDiskFile>();
    file->openFile(path.c_str(), "w+b");
    EXPECT_EQ(0, file->write("disk-data", 9));
    EXPECT_EQ(9U, file->tell());
    EXPECT_EQ(0, file->seek(0));
    char buffer[10] = {0};
    EXPECT_EQ(0, file->read(buffer, 9));
    EXPECT_EQ("disk-data", std::string(buffer, 9));
    EXPECT_EQ(-1, file->read(buffer, 1));
    file->closeFile();
    std::remove(path.c_str());
}

TEST(MP4FileDiskTest, RejectsUnopenablePath) {
    auto file = std::make_shared<TestDiskFile>();
    EXPECT_THROW(file->openFile("/nonexistent-s3mediakit-dir/file.mp4", "wb"), std::runtime_error);
}

#endif
