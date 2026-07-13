#include <gtest/gtest.h>

#include <cstdio>

#include "Local/TimeFile.h"
#include "Local/TimeMaker.h"
#include "Local/TimeDemuxer.h"

using namespace managerkit;

TEST(TimeFileMemoryTest, ReaderWriterSharePositionAndStorage) {
    auto file = std::make_shared<TimeFileMemory>();
    auto writer = file->createWriter();
    auto reader = file->createReader();
    ASSERT_TRUE(writer);
    ASSERT_TRUE(reader);
    EXPECT_EQ(0, writer->write("abcdef", 6));
    EXPECT_EQ(6, writer->tell());
    EXPECT_EQ(6U, file->fileSize());
    EXPECT_EQ(0, writer->flush());
    EXPECT_EQ(-1, writer->seek(7));
    EXPECT_EQ(0, reader->seek(1));

    char buffer[8] = {0};
    EXPECT_EQ(0, reader->read(buffer, 3));
    EXPECT_EQ("bcd", std::string(buffer, 3));
    EXPECT_EQ(4, reader->tell());
    EXPECT_EQ(0, writer->write("XY", 2));
    EXPECT_EQ(6, writer->tell());
    EXPECT_EQ(0, reader->seek(0));
    EXPECT_EQ(0, reader->read(buffer, 8));
    EXPECT_EQ("abcdXY", std::string(buffer, 6));
    EXPECT_EQ(-1, reader->read(buffer, 1));
    EXPECT_EQ("abcdXY", file->getAndClearMemory());
    EXPECT_EQ(0U, file->fileSize());
}

TEST(TimeFileMemoryTest, InitializesFromExistingBuffer) {
    auto file = std::make_shared<TimeFileMemory>("seed");
    EXPECT_EQ(4U, file->fileSize());
    auto reader = file->createReader();
    EXPECT_EQ(4, reader->tell());
    EXPECT_EQ(0, reader->seek(0));
    char data[5] = {0};
    EXPECT_EQ(0, reader->read(data, 4));
    EXPECT_EQ("seed", std::string(data, 4));
}

TEST(TimeFileDiskTest, ReadsWritesFlushesAndRejectsBadPath) {
    const std::string path = "/tmp/s3mediakit-unit-time-file.bin";
    std::remove(path.c_str());
    auto file = std::make_shared<TimeFileDisk>();
    file->openFile(path.c_str(), "w+b");
    auto writer = file->createWriter();
    auto reader = file->createReader();
    EXPECT_EQ(0, writer->write("timeline", 8));
    EXPECT_EQ(0, writer->flush());
    EXPECT_EQ(8, writer->tell());
    EXPECT_EQ(0, reader->seek(0));
    char data[9] = {0};
    EXPECT_EQ(0, reader->read(data, 8));
    EXPECT_EQ("timeline", std::string(data, 8));
    EXPECT_EQ(-1, reader->read(data, 1));
    file->closeFile();
    std::remove(path.c_str());

    auto invalid = std::make_shared<TimeFileDisk>();
    EXPECT_THROW(invalid->openFile("/nonexistent-s3-time-dir/index", "wb"), std::runtime_error);
}

TEST(TimeMakerTest, CreatesMinuteIndexAndTracksOffsets) {
    const std::string path = "/tmp/s3mediakit-unit-timeline.idx";
    std::remove(path.c_str());
    {
        TimeMakerImp maker;
        EXPECT_THROW(maker.openFile(path, "wb"), std::runtime_error);
        maker.openFile(path, "ab+");
        uint64_t t1 = 120;
        size_t size1 = 100;
        EXPECT_TRUE(maker.inputData(t1, size1));
        uint64_t same_minute = 150;
        size_t size2 = 50;
        EXPECT_TRUE(maker.inputData(same_minute, size2));
        uint64_t t2 = 180;
        size_t size3 = 25;
        EXPECT_TRUE(maker.inputData(t2, size3));
        EXPECT_EQ(180U, maker.getLastStamp());
        maker.closeFile();
    }

    {
        TimeMakerImp reader;
        reader.openFile(path, "rb");
        EXPECT_EQ(120U, reader.getFirstStamp());
        BlockListIndexEntry lower = {};
        uint64_t query = 181;
        EXPECT_TRUE(reader.findLowerBound(lower, query));
        EXPECT_EQ(180U, lower.start_time);
        EXPECT_EQ(150U, lower.offset);
        query = 120;
        EXPECT_FALSE(reader.findLowerBound(lower, query));
        reader.closeFile();
    }
    std::remove(path.c_str());
}

TEST(TimeMakerTest, BuildsIndexFileName) {
    EXPECT_EQ("/data/camera.idx", TimeMakerImp::indexFile("/data/camera.s3db"));
    EXPECT_EQ("/data/camera.db", TimeMakerImp::indexFile("/data/camera.db"));
}

namespace {

TimeBlock makeBlock(const std::string &stream, uint64_t start, uint32_t duration) {
    TimeBlock block;
    block.set_app("camera");
    block.set_stream(stream);
    block.set_start_time(start);
    block.set_time_len(duration);
    block.set_file_size(100);
    block.set_file_path("/record/" + stream + ".mp4");
    return block;
}

std::string encodeBlocks(const std::vector<TimeBlock> &blocks) {
    std::string out;
    for (const auto &block : blocks) {
        std::string encoded;
        EXPECT_TRUE(block.SerializeToString(&encoded));
        uint32_t size = encoded.size();
        out.append(reinterpret_cast<const char *>(&size), sizeof(size));
        out += encoded;
    }
    return out;
}

void writeTimeline(const std::string &path, const std::vector<TimeBlock> &blocks, bool index) {
    std::remove(path.c_str());
    std::remove(TimeMakerImp::indexFile(path).c_str());
    auto file = std::make_shared<TimeFileDisk>();
    file->openFile(path.c_str(), "w+b");
    auto writer = file->createWriter();
    TimeMakerImp maker;
    if (index) {
        maker.openFile(TimeMakerImp::indexFile(path), "ab+");
    }
    for (const auto &block : blocks) {
        std::string encoded;
        block.SerializeToString(&encoded);
        uint32_t size = encoded.size();
        if (index) {
            uint64_t start = block.start_time();
            size_t block_size = sizeof(size) + encoded.size();
            maker.inputData(start, block_size);
        }
        writer->write(&size, sizeof(size));
        writer->write(encoded.data(), encoded.size());
    }
    writer->flush();
    file->closeFile();
    maker.closeFile();
}

} // namespace

TEST(TimeMemoryDemuxerTest, ReadsSeeksAndEnforcesReadLimit) {
    auto blocks = std::vector<TimeBlock>{makeBlock("main", 100, 10), makeBlock("main", 200, 20)};
    auto encoded = encodeBlocks(blocks);
    TimeMemoryDemuxer demuxer(encoded);
    EXPECT_EQ(100U, demuxer.getFirstStamp());
    EXPECT_EQ(200, demuxer.seekTo(150));

    TimeBlock block;
    bool eof = false;
    demuxer.readBlock(block, eof);
    EXPECT_FALSE(eof);
    EXPECT_EQ(200U, block.start_time());
    demuxer.readBlock(block, eof);
    EXPECT_TRUE(eof);

    TimeMemoryDemuxer limited(encoded);
    limited.setReadLimit(sizeof(uint32_t) + blocks[0].ByteSizeLong());
    limited.readBlock(block, eof);
    EXPECT_FALSE(eof);
    limited.readBlock(block, eof);
    EXPECT_TRUE(eof);
}

TEST(TimeMemoryDemuxerTest, RejectsMalformedProtobufBlock) {
    uint32_t size = 3;
    std::string malformed(reinterpret_cast<const char *>(&size), sizeof(size));
    malformed += "bad";
    EXPECT_THROW(TimeMemoryDemuxer demuxer(malformed), std::runtime_error);
}

TEST(TimeDemuxerTest, ReadsDiskTimelineWithAndWithoutIndex) {
    const std::string plain = "/tmp/s3mediakit-unit-plain.s3db";
    const std::string indexed = "/tmp/s3mediakit-unit-indexed.s3db";
    auto blocks = std::vector<TimeBlock>{makeBlock("main", 120, 10), makeBlock("main", 180, 10), makeBlock("main", 240, 10)};
    writeTimeline(plain, blocks, false);
    writeTimeline(indexed, blocks, true);

    TimeDemuxer scan;
    scan.openFile(plain);
    EXPECT_EQ(120U, scan.getFirstStamp());
    EXPECT_EQ(240, scan.seekTo(181));
    TimeBlock block;
    bool eof = false;
    scan.readBlock(block, eof);
    EXPECT_EQ(240U, block.start_time());
    scan.closeFile();

    TimeDemuxer with_index;
    with_index.openFile(indexed);
    EXPECT_EQ(120U, with_index.getFirstStamp());
    EXPECT_EQ(180, with_index.seekTo(241));
    with_index.readBlock(block, eof);
    EXPECT_FALSE(eof);
    EXPECT_EQ(180U, block.start_time());
    with_index.closeFile();

    std::remove(plain.c_str());
    std::remove(indexed.c_str());
    std::remove(TimeMakerImp::indexFile(indexed).c_str());
}

TEST(MultiTimeDemuxerTest, SwitchesAcrossTimelineFiles) {
    const std::string first = "/tmp/s3mediakit-unit-multi-a.s3db";
    const std::string second = "/tmp/s3mediakit-unit-multi-b.s3db";
    writeTimeline(first, {makeBlock("main", 100, 10)}, false);
    writeTimeline(second, {makeBlock("main", 300, 10)}, false);

    MultiTimeDemuxer demuxer;
    EXPECT_EQ(-1, demuxer.seekTo(100));
    demuxer.openFile(first + ";" + second);
    EXPECT_EQ(100U, demuxer.getFirstStamp());
    EXPECT_EQ(100, demuxer.seekTo(50));
    TimeBlock block;
    bool eof = false;
    demuxer.readBlock(block, eof);
    EXPECT_FALSE(eof);
    EXPECT_EQ(100U, block.start_time());
    demuxer.readBlock(block, eof);
    EXPECT_FALSE(eof);
    EXPECT_EQ(300U, block.start_time());
    demuxer.readBlock(block, eof);
    EXPECT_TRUE(eof);
    demuxer.closeFile();
    demuxer.readBlock(block, eof);
    EXPECT_TRUE(eof);

    std::remove(first.c_str());
    std::remove(second.c_str());
}
