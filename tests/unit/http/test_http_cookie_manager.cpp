#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "Http/HttpCookieManager.h"

using namespace mediakit;

TEST(RandStrGeneratorTest, ProducesUniqueReusableTokens) {
    RandStrGenerator generator;
    auto first = generator.obtain();
    auto second = generator.obtain();
    EXPECT_EQ(32U, first.size());
    EXPECT_EQ(32U, second.size());
    EXPECT_NE(first, second);
    generator.release(first);
    generator.release(second);
    EXPECT_EQ(32U, generator.obtain().size());
}

TEST(HttpCookieManagerTest, AddsLooksUpFormatsAndDeletesCookie) {
    auto &manager = HttpCookieManager::Instance();
    const std::string name = "UNIT_SESSION_LOOKUP";
    auto cookie = manager.addCookie(name, "user-a", 60, toolkit::Any::make<std::string>("attached"), 2);
    ASSERT_TRUE(cookie);
    EXPECT_EQ(name, cookie->getCookieName());
    EXPECT_EQ("user-a", cookie->getUid());
    EXPECT_FALSE(cookie->getCookie().empty());
    EXPECT_EQ("attached", cookie->getAttach<std::string>());
    EXPECT_FALSE(cookie->isExpired());

    auto plain = cookie->getCookie("/api");
    EXPECT_NE(std::string::npos, plain.find(name + "=" + cookie->getCookie()));
    EXPECT_NE(std::string::npos, plain.find(";path=/api;"));
    auto secure = cookie->getCookie("/");
    EXPECT_EQ(std::string::npos, secure.find("Secure"));
    secure = cookie->getCookie("/", true);
    EXPECT_NE(std::string::npos, secure.find("SameSite=None; Secure; HttpOnly"));

    EXPECT_EQ(cookie, manager.getCookie(name, cookie->getCookie()));
    EXPECT_EQ(cookie, manager.getCookieByUid(name, "user-a"));
    EXPECT_FALSE(manager.getCookie("missing", cookie->getCookie()));
    EXPECT_FALSE(manager.getCookieByUid("", "user-a"));
    EXPECT_FALSE(manager.getCookieByUid(name, ""));
    EXPECT_FALSE(manager.delCookie(HttpServerCookie::Ptr()));
    EXPECT_TRUE(manager.delCookie(cookie));
    EXPECT_FALSE(manager.getCookie(name, cookie->getCookie()));
    EXPECT_FALSE(manager.delCookie(cookie));
}

TEST(HttpCookieManagerTest, ExtractsCookieFromRequestHeader) {
    auto &manager = HttpCookieManager::Instance();
    const std::string name = "UNIT_HEADER_COOKIE";
    auto cookie = manager.addCookie(name, "user-header", 60);
    StrCaseMap headers;
    EXPECT_FALSE(manager.getCookie(name, headers));
    headers["Cookie"] = "other=x; " + name + "=" + cookie->getCookie() + "; tail=y";
    EXPECT_EQ(cookie, manager.getCookie(name, headers));
    headers["Cookie"] = name + "=" + cookie->getCookie();
    EXPECT_EQ(cookie, manager.getCookie(name, headers));
    headers["Cookie"] = "other=x";
    EXPECT_FALSE(manager.getCookie(name, headers));
    EXPECT_TRUE(manager.delCookie(cookie));
}

TEST(HttpCookieManagerTest, EnforcesPerUserClientLimit) {
    auto &manager = HttpCookieManager::Instance();
    const std::string name = "UNIT_CLIENT_LIMIT";
    auto first = manager.addCookie(name, "same-user", 60, toolkit::Any{}, 1);
    ASSERT_TRUE(first);
    auto first_token = first->getCookie();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto second = manager.addCookie(name, "same-user", 60, toolkit::Any{}, 1);
    ASSERT_TRUE(second);
    EXPECT_NE(first_token, second->getCookie());
    EXPECT_FALSE(manager.getCookie(name, first_token));
    // Releasing the displaced object removes its UID index entry; callers may
    // still hold the old object even though its token is already invalid.
    first.reset();
    EXPECT_EQ(second, manager.getCookieByUid(name, "same-user"));
    EXPECT_TRUE(manager.delCookie(second));
}

TEST(HttpCookieManagerTest, ExpiresShortLivedCookieAndRefreshesTime) {
    auto &manager = HttpCookieManager::Instance();
    const std::string name = "UNIT_EXPIRED_COOKIE";
    auto cookie = manager.addCookie(name, "expiring", 0);
    ASSERT_TRUE(cookie);
    cookie->updateTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_TRUE(cookie->isExpired());
    EXPECT_FALSE(manager.getCookie(name, cookie->getCookie()));
}
