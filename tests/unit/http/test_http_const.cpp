#include <gtest/gtest.h>

#include "Http/HttpConst.h"

using mediakit::HttpConst;

TEST(HttpConstTest, ReturnsStatusMessages) {
    EXPECT_STREQ("Continue", HttpConst::getHttpStatusMessage(100));
    EXPECT_STREQ("OK", HttpConst::getHttpStatusMessage(200));
    EXPECT_STREQ("Not Found", HttpConst::getHttpStatusMessage(404));
    EXPECT_STREQ("Internal Server Error", HttpConst::getHttpStatusMessage(500));
    EXPECT_STREQ("Internal Server Error", HttpConst::getHttpStatusMessage(999));
}

TEST(HttpConstTest, ResolvesMimeTypesCaseInsensitively) {
    EXPECT_EQ("text/html", HttpConst::getHttpContentType("index.HTML"));
    EXPECT_EQ("application/json", HttpConst::getHttpContentType("data.json"));
    EXPECT_EQ("video/mp4", HttpConst::getHttpContentType("clip.mp4"));
    EXPECT_EQ("text/plain", HttpConst::getHttpContentType("README"));
    EXPECT_EQ("text/plain", HttpConst::getHttpContentType("file.unknown"));
}
