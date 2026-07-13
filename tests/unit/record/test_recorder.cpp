#include <gtest/gtest.h>

#include "Record/Recorder.h"
#include "Common/config.h"

using namespace mediakit;

TEST(RecorderTest, BuildsCustomizedRecordPaths) {
    MediaTuple tuple("tenant", "live", "camera/main", "");
    auto hls = Recorder::getRecordPath(Recorder::type_hls, tuple, "/tmp/s3-unit-records");
    auto fmp4 = Recorder::getRecordPath(Recorder::type_hls_fmp4, tuple, "/tmp/s3-unit-records");
    auto mp4 = Recorder::getRecordPath(Recorder::type_mp4, tuple, "/tmp/s3-unit-records");

    EXPECT_EQ("/tmp/s3-unit-records/live/camera/main/hls.m3u8", hls);
    EXPECT_EQ("/tmp/s3-unit-records/live/camera/main/hls.fmp4.m3u8", fmp4);
    EXPECT_NE(std::string::npos, mp4.find("/live/camera/main/"));
    EXPECT_EQ("", Recorder::getRecordPath(static_cast<Recorder::type>(99), tuple, "/tmp"));
}

TEST(RecorderTest, BuildsConfiguredDefaultPaths) {
    MediaTuple tuple(DEFAULT_VHOST, "app", "stream", "");
    EXPECT_NE(std::string::npos, Recorder::getRecordPath(Recorder::type_hls, tuple).find("app/stream/hls.m3u8"));
    EXPECT_NE(std::string::npos, Recorder::getRecordPath(Recorder::type_hls_fmp4, tuple).find("app/stream/hls.fmp4.m3u8"));
    EXPECT_NE(std::string::npos, Recorder::getRecordPath(Recorder::type_mp4, tuple).find("app/stream/"));
}
