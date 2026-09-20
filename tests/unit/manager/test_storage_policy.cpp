#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include "Common/StrUtil.h"
#include "Local/StoragePolicy.h"
#include "Util/File.h"

using namespace managerkit;

namespace {

// Build "<root>/<camera>/<stream>/<date>/<name>.mp4" the way the recorder lays segments out.
void makeSegment(const std::string &root, const std::string &key, const std::string &date,
                 const std::string &name) {
    const std::string path = root + "/" + key + "/" + date + "/" + name + ".mp4";
    toolkit::File::create_path(path, 0777);
    FILE *fp = toolkit::File::create_file(path, "wb");
    ASSERT_NE(nullptr, fp);
    fputs("x", fp);
    fclose(fp);
}

std::string makeTempRoot(const char *tag) {
    std::string tmpl = std::string("/tmp/s3mediakit-unit-") + tag + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    EXPECT_NE(nullptr, ::mkdtemp(&buf[0]));
    return std::string(&buf[0]);
}

uint64_t dateTs(const char *date) {
    return StrTimeUtils::getTsFromDateStr(date);
}

} // namespace

TEST(StoragePolicyFloorTest, ReturnsTheOldestDateDirectoryOnDisk) {
    const std::string root = makeTempRoot("oldest-date");
    const std::string key = "cam-001/main";
    makeSegment(root, key, "2026-09-18", "2026-09-18-10-00-00");
    makeSegment(root, key, "2026-09-11", "2026-09-11-23-59-00");
    makeSegment(root, key, "2026-09-15", "2026-09-15-00-00-30");

    EXPECT_EQ(dateTs("2026-09-11"), oldestSegmentDate(root, key));

    toolkit::File::delete_file(root, true);
}

TEST(StoragePolicyFloorTest, EachStreamOfACameraKeepsItsOwnFloor) {
    // max_value is computed once per camera but the floor is per stream, which is why the clamp
    // is applied at the point where each stream is emplaced.
    const std::string root = makeTempRoot("per-stream");
    makeSegment(root, "cam-002/main", "2026-09-03", "2026-09-03-01-00-00");
    makeSegment(root, "cam-002/sub", "2026-09-17", "2026-09-17-01-00-00");

    EXPECT_EQ(dateTs("2026-09-03"), oldestSegmentDate(root, "cam-002/main"));
    EXPECT_EQ(dateTs("2026-09-17"), oldestSegmentDate(root, "cam-002/sub"));

    toolkit::File::delete_file(root, true);
}

TEST(StoragePolicyFloorTest, ReturnsZeroWhenTheStreamHasNothingOnDisk) {
    const std::string root = makeTempRoot("no-segment");

    // Stream directory absent entirely.
    EXPECT_EQ(0U, oldestSegmentDate(root, "cam-003/main"));

    // Stream directory present but empty.
    toolkit::File::create_path((root + "/cam-003/sub/"), 0777);
    EXPECT_EQ(0U, oldestSegmentDate(root, "cam-003/sub"));

    toolkit::File::delete_file(root, true);
}

TEST(StoragePolicyFloorTest, IgnoresDirectoryNamesThatAreNotDates) {
    const std::string root = makeTempRoot("junk-dir");
    const std::string key = "cam-004/main";
    makeSegment(root, key, "2026-09-14", "2026-09-14-08-00-00");
    // The archive subtree and any stray directory must not be mistaken for a date.
    toolkit::File::create_path((root + "/" + key + "/archive/"), 0777);
    toolkit::File::create_path((root + "/" + key + "/not-a-date/"), 0777);

    EXPECT_EQ(dateTs("2026-09-14"), oldestSegmentDate(root, key));

    toolkit::File::delete_file(root, true);
}

TEST(StoragePolicyThresholdTest, InterpolatesBetweenTheTwoBounds) {
    const uint64_t newer = 2000;
    const uint64_t older = 1000;

    EXPECT_EQ(2000U, keepTimeThreshold(newer, older, 0));
    EXPECT_EQ(1500U, keepTimeThreshold(newer, older, 50));
    EXPECT_EQ(1000U, keepTimeThreshold(newer, older, 100));
}

TEST(StoragePolicyThresholdTest, DoesNotWrapWhenTheBoundsArriveInverted) {
    // keepArchivedMinFor > keepArchivedMaxFor puts the older timestamp first. The previous
    // unsigned subtraction wrapped here and produced a threshold far in the future, which would
    // delete every segment of the stream.
    const uint64_t min_value = 1000; // older than max_value: inverted
    const uint64_t max_value = 2000;

    for (int pct = 0; pct <= 100; pct += 25) {
        uint64_t threshold = keepTimeThreshold(min_value, max_value, pct);
        EXPECT_GE(threshold, min_value) << "keep_percent=" << pct;
        EXPECT_LE(threshold, max_value) << "keep_percent=" << pct;
    }
}

TEST(StoragePolicyThresholdTest, ClampsKeepPercentOutOfRange) {
    EXPECT_EQ(2000U, keepTimeThreshold(2000, 1000, -20));
    EXPECT_EQ(1000U, keepTimeThreshold(2000, 1000, 140));
}

TEST(StoragePolicyThresholdTest, HandlesEqualBounds) {
    EXPECT_EQ(1500U, keepTimeThreshold(1500, 1500, 50));
}
