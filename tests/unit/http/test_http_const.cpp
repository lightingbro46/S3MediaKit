#include <gtest/gtest.h>

#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Http/HttpConst.h"
#include "Http/HttpFileManager.h"
#include "Util/mini.h"

using mediakit::HttpConst;
using mediakit::HttpFileManager;

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

TEST(HttpFileManagerTest, TrustsOnlyConfiguredOriginalUrlProxyRanges) {
    toolkit::mINI::Instance()[mediakit::Http::kOriginalUrlTrustedProxy] =
        "127.0.0.1,10.0.0.1-10.0.0.3,::1";

    EXPECT_TRUE(HttpFileManager::isOriginalUrlTrustedProxy("127.0.0.1"));
    EXPECT_TRUE(HttpFileManager::isOriginalUrlTrustedProxy("10.0.0.2"));
    EXPECT_TRUE(HttpFileManager::isOriginalUrlTrustedProxy("::1"));
    EXPECT_FALSE(HttpFileManager::isOriginalUrlTrustedProxy("10.0.0.4"));
    EXPECT_FALSE(HttpFileManager::isOriginalUrlTrustedProxy("192.0.2.1"));
}
