#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "Storage/UserSession.h"

using namespace managerkit;

// Runs inside unit_video_config_rule_job, whose environment builds a temporary mserver DB from the
// real migrations. Sessions are keyed by the JWT session_id claim; the JWT itself is never stored.

TEST(UserSessionImpTest, StoresSessionIdNotToken) {
    auto columns = EntityTraits<UserSession>::getColumns();
    EXPECT_EQ(columns.end(), std::find(columns.begin(), columns.end(), "token"));
    EXPECT_STREQ("user_auth_sessions", EntityTraits<UserSession>::tableName().c_str());

    auto imp = std::make_shared<UserSessionImp>();
    UserSession session;
    session.sessionId = "sess-store-1";
    session.userId = "user-1";
    session.creationTimeS = 1000;
    imp->add(session, "");
    imp->add(session, "");

    auto rows = imp->findById("sess-store-1");
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ("user-1", rows[0].userId);
    EXPECT_EQ(1000, rows[0].creationTimeS);
}

TEST(UserSessionImpTest, SkipsSessionWithoutId) {
    auto imp = std::make_shared<UserSessionImp>();
    UserSession session;
    session.userId = "user-no-session";
    session.creationTimeS = 1000;
    imp->add(session, "");
    EXPECT_TRUE(imp->findById("").empty());
}

TEST(UserSessionImpTest, RemovesSessionsOlderThanStamp) {
    auto imp = std::make_shared<UserSessionImp>();
    UserSession old_session;
    old_session.sessionId = "sess-old";
    old_session.userId = "user-2";
    old_session.creationTimeS = 100;
    imp->add(old_session, "");
    UserSession new_session = old_session;
    new_session.sessionId = "sess-new";
    new_session.creationTimeS = 5000;
    imp->add(new_session, "");

    EXPECT_TRUE(imp->removeByStamp(2000));
    EXPECT_TRUE(imp->findById("sess-old").empty());
    EXPECT_EQ(1u, imp->findById("sess-new").size());
}
