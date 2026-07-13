#include <gtest/gtest.h>

#include "Http/HttpCookie.h"

using mediakit::HttpCookie;

TEST(HttpCookieTest, StoresKeyValueAndValidity) {
    HttpCookie cookie;
    EXPECT_FALSE(static_cast<bool>(cookie));
    cookie.setKeyVal("session", "abc");
    cookie.setHost("example.com");
    cookie.setPath("/api");
    cookie.setExpires("Wed, Jan 01 2037 00:00:00 GMT", "");
    EXPECT_TRUE(static_cast<bool>(cookie));
    EXPECT_EQ("session", cookie.getKey());
    EXPECT_EQ("abc", cookie.getVal());
}
