#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "Common/StrUtil.h"

using managerkit::StrJsonUtils;
using managerkit::StampUtils;
using managerkit::StrTimeUtils;
using managerkit::StrUUID;
using managerkit::UriUtils;

TEST(UriUtilsTest, BuildsUrisInPreferredOrder) {
    const std::vector<std::string> uris =
        UriUtils::getUriList("media.example.com", "192.0.2.10", 80, 443, true);

    ASSERT_EQ(3U, uris.size());
    EXPECT_EQ("https://media.example.com:443", uris[0]);
    EXPECT_EQ("http://media.example.com:80", uris[1]);
    EXPECT_EQ("http://192.0.2.10:80", uris[2]);
}

TEST(UriUtilsTest, OmitsEmptyDomainAndIp) {
    EXPECT_TRUE(UriUtils::getUriList("", "", 80, 443, true).empty());
}

TEST(UriUtilsTest, RewritesAddressParts) {
    EXPECT_EQ("rtsp://203.0.113.5:554/live/main",
              UriUtils::replaceIp("rtsp://192.0.2.1:554/live/main", "203.0.113.5"));
    EXPECT_EQ("rtsp://camera.local:8554/live/main",
              UriUtils::replacePort("rtsp://camera.local:554/live/main", 8554));
    EXPECT_EQ("rtsp://new:secret@camera.local/live",
              UriUtils::replaceCredentials("rtsp://old:pwd@camera.local/live", "new", "secret"));
    EXPECT_EQ("rtsp://camera.local/live",
              UriUtils::replaceCredentials("rtsp://old:pwd@camera.local/live", "", ""));
}

TEST(StrJsonUtilsTest, RoundTripsJsonValue) {
    Json::Value input;
    input["name"] = "camera-01";
    input["enabled"] = true;
    input["priority"] = 3;

    Json::Value output;
    ASSERT_TRUE(StrJsonUtils::readJsonString(StrJsonUtils::writeJsonString(input), output));
    EXPECT_EQ(input, output);
}

TEST(StrJsonUtilsTest, RejectsMalformedJson) {
    Json::Value output;
    EXPECT_FALSE(StrJsonUtils::readJsonString("{not-json}", output));
}

TEST(StrTimeUtilsTest, RejectsInvalidInputAndParsesSupportedFormats) {
    EXPECT_EQ(0U, StrTimeUtils::getTsFromDateStr(""));
    EXPECT_EQ(0U, StrTimeUtils::getTsFromDateStr("2026/07/12"));
    EXPECT_GT(StrTimeUtils::getTsFromDateStr("2026-07-12"), 0U);
    EXPECT_GT(StrTimeUtils::getTsFromDateTimeStr("2026-07-12/12-34-56-extra"), 0U);
    EXPECT_EQ(0U, StrTimeUtils::getTsFromDateTimeStr("2026-07-12"));
    EXPECT_GT(StrTimeUtils::getTsFromDateTimeStr2("2026-07-12/2026-07-12-12-34-56-1"), 0U);
    EXPECT_EQ(0U, StrTimeUtils::getTsFromDateTimeStr2("bad/input"));
}

TEST(StampUtilsTest, FloorsTimestampToCalendarBoundaries) {
    std::tm tm = {};
    tm.tm_year = 126;
    tm.tm_mon = 6;
    tm.tm_mday = 12;
    tm.tm_hour = 13;
    tm.tm_min = 27;
    tm.tm_sec = 42;
    tm.tm_isdst = -1;
    const uint64_t stamp = static_cast<uint64_t>(std::mktime(&tm));

    EXPECT_LE(StampUtils::getStartOfMinute(stamp), stamp);
    EXPECT_LE(StampUtils::getStartOfHour(stamp), StampUtils::getStartOfMinute(stamp));
    EXPECT_LE(StampUtils::getStartOfDay(stamp), StampUtils::getStartOfHour(stamp));
}

// Timeline handlers, tiering and the recorder floor stamps on many threads at once.
TEST(StampUtilsTest, FloorsConsistentlyAcrossThreads) {
    std::vector<uint64_t> stamps;
    for (uint64_t i = 0; i < 64; ++i) {
        stamps.push_back(1767225600ULL + i * 86400 * 7 + i * 3671);
    }
    std::vector<uint64_t> days, hours;
    for (auto stamp : stamps) {
        days.push_back(StampUtils::getStartOfDay(stamp));
        hours.push_back(StampUtils::getStartOfHour(stamp));
    }

    std::vector<int> mismatches(8, 0);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < mismatches.size(); ++t) {
        threads.emplace_back([&, t]() {
            for (int round = 0; round < 2000; ++round) {
                auto i = (t * 7 + round) % stamps.size();
                if (StampUtils::getStartOfDay(stamps[i]) != days[i] || StampUtils::getStartOfHour(stamps[i]) != hours[i]) {
                    ++mismatches[t];
                }
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    for (auto count : mismatches) {
        EXPECT_EQ(0, count);
    }
}

TEST(StrUUIDTest, AppliesRequestedLengthAndAffixes) {
    const std::string value = StrUUID::make_guid(8, "pre", "post");
    EXPECT_EQ(17U, value.size());
    EXPECT_EQ("pre-", value.substr(0, 4));
    EXPECT_EQ("-post", value.substr(value.size() - 5));
    EXPECT_NE(StrUUID::make_guid(), StrUUID::make_guid());
}
