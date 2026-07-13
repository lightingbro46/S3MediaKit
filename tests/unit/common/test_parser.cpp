#include <gtest/gtest.h>

#include "Common/Parser.h"

using namespace mediakit;

TEST(ParserHelpersTest, ExtractsSubstringsAndSplitsUrls) {
    EXPECT_EQ("value", findSubString("prefix[value]suffix", "[", "]"));
    EXPECT_EQ("prefix", findSubString("prefix[value]", nullptr, "["));

    std::string host;
    uint16_t port = 0;
    splitUrl("example.com:8554", host, port);
    EXPECT_EQ("example.com", host);
    EXPECT_EQ(8554, port);

    splitUrl("[2001:db8::1]:443", host, port);
    EXPECT_EQ("2001:db8::1", host);
    EXPECT_EQ(443, port);
}

TEST(ParserTest, ParsesRequestHeadersBodyAndArguments) {
    const std::string raw =
        "POST /api/items?a=1&Name=Camera HTTP/1.1\r\n"
        "Host: example.com\r\nContent-Length: 4\r\n\r\nbody";
    Parser parser;
    parser.parse(raw.c_str(), raw.size());

    EXPECT_EQ("POST", parser.method());
    EXPECT_EQ("/api/items", parser.url());
    EXPECT_EQ("a=1&Name=Camera", parser.params());
    EXPECT_EQ("HTTP/1.1", parser.protocol());
    EXPECT_EQ("example.com", parser["host"]);
    EXPECT_EQ("body", parser.content());
    EXPECT_EQ("Camera", parser.getUrlArgs()["name"]);
}

TEST(ParserTest, ParsesResponseAndCanBeReused) {
    const std::string raw = "HTTP/1.1 404 Not Found\r\nX-Test: yes\r\n\r\n";
    Parser parser;
    parser.parse(raw.c_str(), raw.size());
    EXPECT_EQ("404", parser.status());
    EXPECT_EQ("Not Found", parser.statusStr());

    parser.clear();
    parser.setUrl("/changed?q=x");
    parser.setContent("payload");
    EXPECT_EQ("/changed?q=x", parser.url());
    EXPECT_EQ("payload", parser.content());
}

TEST(ParserTest, ParsesAndMergesArgumentsAndUrls) {
    StrCaseMap args = Parser::parseArgs("A=1&empty=&flag");
    EXPECT_EQ("1", args["a"]);
    EXPECT_EQ("", args["empty"]);
    EXPECT_EQ("", args["flag"]);
    EXPECT_EQ("http://example.com/a/c", Parser::mergeUrl("http://example.com/a/b", "c"));
}

TEST(RtspUrlTest, ParsesCredentialsIpv4AndIpv6) {
    RtspUrl url;
    url.parse("rtsps://user:pass@example.com:8554/live");
    EXPECT_TRUE(url._is_ssl);
    EXPECT_EQ("example.com", url._host);
    EXPECT_EQ(8554, url._port);
    EXPECT_EQ("user", url._user);
    EXPECT_EQ("pass", url._passwd);

    url.parse("rtsp://[2001:db8::2]/stream");
    EXPECT_FALSE(url._is_ssl);
    EXPECT_EQ("2001:db8::2", url._host);
    EXPECT_EQ(554, url._port);
}
