#include <gtest/gtest.h>

#include "Common/strCoding.h"

using namespace mediakit;

TEST(StrCodingTest, EncodesDifferentUrlComponents) {
    EXPECT_EQ("folder/a%20b?q=1&x=2", strCoding::UrlEncodePath("folder/a b?q=1&x=2"));
    EXPECT_EQ("folder%2Fa%20b%3Fq%3D1%26x%3D2",
              strCoding::UrlEncodeComponent("folder/a b?q=1&x=2"));
    EXPECT_EQ("user%40host%3Ap%2Fq%3Fr", strCoding::UrlEncodeUserOrPass("user@host:p/q?r"));
    EXPECT_EQ("caf%C3%A9", strCoding::UrlEncodeComponent("caf\xC3\xA9"));
}

TEST(StrCodingTest, DecodesPathComponentAndUserInfo) {
    EXPECT_EQ("folder%2Fa b%3Fq%3D1", strCoding::UrlDecodePath("folder%2Fa%20b%3Fq%3D1"));
    EXPECT_EQ("a b+c/d?e", strCoding::UrlDecodeComponent("a+b%2Bc%2Fd%3Fe"));
    EXPECT_EQ("user@host:p/q?r", strCoding::UrlDecodeUserOrPass("user%40host%3Ap%2Fq%3Fr"));
    EXPECT_EQ("caf\xC3\xA9", strCoding::UrlDecodeComponent("caf%C3%A9"));
}

TEST(StrCodingTest, PreservesMalformedEscapes) {
    EXPECT_EQ("abc%", strCoding::UrlDecodeComponent("abc%"));
    EXPECT_EQ("abc%2", strCoding::UrlDecodeComponent("abc%2"));
    EXPECT_EQ("abc%GGdef", strCoding::UrlDecodeComponent("abc%GGdef"));
    EXPECT_EQ("%2F%3F", strCoding::UrlDecodePath("%2F%3F"));
    EXPECT_EQ("", strCoding::UrlEncodePath(""));
}
