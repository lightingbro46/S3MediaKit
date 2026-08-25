#include <gtest/gtest.h>

#include "Common/Parser.h"
#include "Http/HlsViewerSession.h"

using namespace mediakit;

TEST(HlsViewerSessionTest, AcceptsOnlySafeBoundedSessionIds) {
    EXPECT_TRUE(HlsViewerSession::isValidSessionId("viewer-1_abc.xyz~9"));
    EXPECT_FALSE(HlsViewerSession::isValidSessionId(""));
    EXPECT_FALSE(HlsViewerSession::isValidSessionId("viewer/id"));
    EXPECT_FALSE(HlsViewerSession::isValidSessionId(std::string(129, 'a')));
}

TEST(HlsViewerSessionTest, ReadsSessionIdFromQuery) {
    Parser parser;
    const char request[] = "GET /live/hls.m3u8?token=secret&session_id=viewer-1 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    parser.parse(request, sizeof(request) - 1);
    EXPECT_EQ("viewer-1", HlsViewerSession::getSessionId(parser));
}

TEST(HlsViewerSessionTest, ResolvesQueryBeforeCookie) {
    Parser parser;
    const char request[] = "GET /live/hls.m3u8?session_id=query-viewer HTTP/1.1\r\nHost: example.com\r\n\r\n";
    parser.parse(request, sizeof(request) - 1);

    auto identity = HlsViewerSession::resolve(parser, "cookie-viewer");
    EXPECT_EQ(HlsViewerSession::Identity::Query, identity.origin);
    EXPECT_EQ("query-viewer", identity.session_id);
}

TEST(HlsViewerSessionTest, FallsBackToCookieOrGeneratedIdentity) {
    Parser parser;
    const char request[] = "GET /live/hls.m3u8 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    parser.parse(request, sizeof(request) - 1);

    auto cookie_identity = HlsViewerSession::resolve(parser, "cookie-viewer");
    EXPECT_EQ(HlsViewerSession::Identity::Cookie, cookie_identity.origin);
    EXPECT_EQ("cookie-viewer", cookie_identity.session_id);

    auto generated_identity = HlsViewerSession::resolve(parser, "");
    EXPECT_EQ(HlsViewerSession::Identity::Generated, generated_identity.origin);
    EXPECT_TRUE(HlsViewerSession::isValidSessionId(generated_identity.session_id));
}

TEST(HlsViewerSessionTest, RejectsInvalidExplicitIdentityWithoutCookieFallback) {
    Parser parser;
    const char request[] = "GET /live/hls.m3u8?session_id=bad/value HTTP/1.1\r\nHost: example.com\r\n\r\n";
    parser.parse(request, sizeof(request) - 1);

    auto identity = HlsViewerSession::resolve(parser, "cookie-viewer");
    EXPECT_EQ(HlsViewerSession::Identity::Invalid, identity.origin);
    EXPECT_TRUE(identity.session_id.empty());
}

TEST(HlsViewerSessionTest, AddsOrReplacesSessionIdWithoutChangingFragment) {
    EXPECT_EQ("segment.ts?session_id=viewer-1",
              HlsViewerSession::appendSessionIdToUri("segment.ts", "viewer-1"));
    EXPECT_EQ("segment.ts?token=x&session_id=viewer-1#part",
              HlsViewerSession::appendSessionIdToUri("segment.ts?token=x#part", "viewer-1"));
    EXPECT_EQ("segment.ts?session_id=viewer-1&token=x",
              HlsViewerSession::appendSessionIdToUri("segment.ts?session_id=old&token=x", "viewer-1"));
}

TEST(HlsViewerSessionTest, BuildsRedirectUrlWithOriginalPrefixAndExistingParameters) {
    const char request[] =
        "GET /media/live/camera/main/hls.m3u8?token=secret&transcode=true HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n";
    Parser parser;
    parser.parse(request, sizeof(request) - 1);
    ASSERT_TRUE(parser.setOriginalUrl("/media2/media/live/camera/main/hls.m3u8?token=secret&transcode=true"));

    auto headers = HlsViewerSession::makeHlsPlaylistRedirectHeader(
        parser.toOriginalUrl(parser.url()), parser.params(), "viewer-1");
    EXPECT_EQ("/media2/media/live/camera/main/hls.m3u8?token=secret&transcode=true&session_id=viewer-1",
              headers["Location"]);
    EXPECT_EQ("no-store", headers["Cache-Control"]);
    EXPECT_EQ("/media2/media/live/camera/main_transcode/hls.m3u8?token=secret&transcode=true&session_id=viewer-1",
              HlsViewerSession::makePlaylistRedirectUrl(
                  parser.toOriginalUrl("/media/live/camera/main_transcode/hls.m3u8"), parser.params(), "viewer-1"));
}

TEST(HlsViewerSessionTest, ReplacesSessionIdWhenBuildingRedirectUrl) {
    const char request[] =
        "GET /media/live/camera/main/hls.m3u8?session_id=old&token=secret HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n";
    Parser parser;
    parser.parse(request, sizeof(request) - 1);

    EXPECT_EQ("/media/live/camera/main/hls.m3u8?session_id=viewer-1&token=secret",
              HlsViewerSession::makePlaylistRedirectUrl(
                  parser.toOriginalUrl(parser.url()), parser.params(), "viewer-1"));
}

TEST(HlsViewerSessionTest, RewritesMediaAndMasterPlaylistUris) {
    const std::string playlist =
        "#EXTM3U\r\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\r\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"https://keys.example/key?id=7\"\r\n"
        "#EXTINF:2.0,\r\n"
        "segment0.ts\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000\r\n"
        "/media/camera/low/hls.m3u8?quality=lo\r\n";

    auto rewritten = HlsViewerSession::rewritePlaylist(playlist, "viewer-1");
    EXPECT_NE(std::string::npos, rewritten.find("URI=\"init.mp4?session_id=viewer-1\""));
    EXPECT_NE(std::string::npos, rewritten.find("URI=\"https://keys.example/key?id=7&session_id=viewer-1\""));
    EXPECT_NE(std::string::npos, rewritten.find("segment0.ts?session_id=viewer-1\r\n"));
    EXPECT_NE(std::string::npos,
              rewritten.find("/media/camera/low/hls.m3u8?quality=lo&session_id=viewer-1\r\n"));
    EXPECT_EQ(std::string::npos, rewritten.find("token=secret"));
}
