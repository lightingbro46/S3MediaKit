#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "Common/config.h"
#include "FFmpegSource.h"
#include "Camera/StreamSource.h"
#include "Local/TimeFile.h"
#include "Local/TimeMaker.h"
#include "Local/StatisticRecorder.h"
#include "Util/File.h"

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

namespace {

TimeBlock makeBlock(const std::string &stream, uint64_t start, uint32_t duration) {
    TimeBlock block;
    block.set_app("extract-preview-camera");
    block.set_stream(stream);
    block.set_start_time(start);
    block.set_time_len(duration);
    block.set_file_size(100);
    block.set_file_path("/record/" + stream + ".mp4");
    return block;
}

void writeTimeline(const std::string &path, const std::vector<TimeBlock> &blocks) {
    std::remove(path.c_str());
    std::remove(TimeMakerImp::indexFile(path).c_str());
    auto file = std::make_shared<TimeFileDisk>();
    file->openFile(path.c_str(), "w+b");
    auto writer = file->createWriter();
    TimeMakerImp maker;
    maker.openFile(TimeMakerImp::indexFile(path), "ab+");
    for (const auto &block : blocks) {
        std::string encoded;
        ASSERT_TRUE(block.SerializeToString(&encoded));
        uint32_t size = encoded.size();
        uint64_t start = block.start_time();
        size_t block_size = sizeof(size) + encoded.size();
        ASSERT_TRUE(maker.inputData(start, block_size));
        ASSERT_EQ(0, writer->write(&size, sizeof(size)));
        ASSERT_EQ(0, writer->write(encoded.data(), encoded.size()));
    }
    writer->flush();
    file->closeFile();
    maker.closeFile();
}

class ExtractPreviewTest : public ::testing::Test {
protected:
    void SetUp() override {
        record_root = "/tmp/s3mediakit-extract-preview";
        record_path = File::absolutePath("record", record_root);
        File::create_path(record_path, 0777);
        mINI::Instance()[Protocol::kMP4SavePath] = record_root;
        mINI::Instance()[Record::kAppName] = "record";
        auto recorder = StatisticRecorder::Instance().getRecorder("extract-preview-camera", true);
        StreamTuple main_stream;
        main_stream.stream_id = "main";
        StreamTuple sub_stream;
        sub_stream.stream_id = "sub";
        recorder->setStreamTuples({
            { PrimaryStream, main_stream },
            { SecondaryStream, sub_stream }
        });
        const uint64_t now = time(nullptr);
        recorder->addArchiveSize("main", 1, 100, now - 60, now + 60);
        recorder->addArchiveSize("sub", 1, 100, now - 50, now + 60);
        timeline_path = File::absolutePath("extract-preview-camera/extract-preview-camera.s3db", record_path);

    }

    void TearDown() override {
        std::remove(timeline_path.c_str());
        std::remove(TimeMakerImp::indexFile(timeline_path).c_str());
    }

    std::string record_root;
    std::string record_path;
    std::string timeline_path;
};

TEST_F(ExtractPreviewTest, ReportsSelectedStreamAndAvailableDuration) {
    const uint64_t now = time(nullptr);
    writeTimeline(timeline_path, {
        makeBlock("main", now - 10, 20),
        makeBlock("main", now + 30, 10),
        makeBlock("sub", now, 60),
    });

    auto preview = FFmpegExtractor::getExtractPreview("extract-preview-camera", "", now, now + 50);

    EXPECT_TRUE(preview.hasData);
    EXPECT_EQ(50U, preview.availableDuration);
    EXPECT_EQ("sub", preview.streamId);
}

TEST_F(ExtractPreviewTest, ReportsNoDataForEmptyRange) {
    const uint64_t now = time(nullptr);
    writeTimeline(timeline_path, {makeBlock("main", now - 20, 10)});

    auto preview = FFmpegExtractor::getExtractPreview("extract-preview-camera", "", now + 100, now + 200);

    EXPECT_FALSE(preview.hasData);
    EXPECT_EQ(0U, preview.availableDuration);
    EXPECT_TRUE(preview.streamId.empty());
    EXPECT_TRUE(preview.streamName.empty());
}

} // namespace