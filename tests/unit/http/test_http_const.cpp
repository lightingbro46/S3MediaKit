#include <gtest/gtest.h>

#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Http/HttpConst.h"
#include "Http/HttpFileManager.h"
#include "Http/HlsViewerSession.h"
#include "Util/mini.h"

using mediakit::HttpConst;
using mediakit::HttpFileManager;
using mediakit::HlsViewerSession;
using mediakit::MediaInfo;
using mediakit::Parser;

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

TEST(HttpFileManagerTest, ResolvesGeneratedQueryAndInvalidHlsIdentities) {
    MediaInfo media_info("hls://example.com/camera/main");

    const char missing_request[] =
        "GET /media/live/camera/main/hls.m3u8 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    Parser missing_parser;
    missing_parser.parse(missing_request, sizeof(missing_request) - 1);
    auto generated = HttpFileManager::resolveHlsViewerIdentity(missing_parser, media_info);
    EXPECT_EQ(HlsViewerSession::Identity::Generated, generated.origin);
    EXPECT_TRUE(HlsViewerSession::isValidSessionId(generated.session_id));

    const char query_request[] =
        "GET /media/live/camera/main/hls.m3u8?session_id=viewer-1 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    Parser query_parser;
    query_parser.parse(query_request, sizeof(query_request) - 1);
    auto query = HttpFileManager::resolveHlsViewerIdentity(query_parser, media_info);
    EXPECT_EQ(HlsViewerSession::Identity::Query, query.origin);
    EXPECT_EQ("viewer-1", query.session_id);

    const char invalid_request[] =
        "GET /media/live/camera/main/hls.m3u8?session_id=bad/value HTTP/1.1\r\nHost: example.com\r\n\r\n";
    Parser invalid_parser;
    invalid_parser.parse(invalid_request, sizeof(invalid_request) - 1);
    auto invalid = HttpFileManager::resolveHlsViewerIdentity(invalid_parser, media_info);
    EXPECT_EQ(HlsViewerSession::Identity::Invalid, invalid.origin);
    EXPECT_TRUE(invalid.session_id.empty());
}
