#include <gtest/gtest.h>

#include "Common/StrUtil.h"

using managerkit::StrJsonUtils;
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
