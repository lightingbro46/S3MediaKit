#ifndef S3MANAGERKIT_USERSESSION_H
#define S3MANAGERKIT_USERSESSION_H

#include <string>
#include "DbStorage.h"
#include "Util/util.h"

namespace managerkit { 

struct UserSession {
    std::string token;
    Optional<std::string> userId;
    int64_t creationTimeS;
};

DECLARE_ENTITY(UserSession, "user_sessions",
    {"token"},
    &UserSession::token, "token", 
    &UserSession::userId, "userId", 
    &UserSession::creationTimeS, "creationTimeS", 
)

class UserSessionRepository : public SqliteRepository<UserSession> {
public:
    UserSessionRepository() : SqliteRepository<UserSession>(Database::kMediaServerDb) {}

};

class UserSessionImp : public UserSessionRepository {
public:
    using Ptr = std::shared_ptr<UserSessionImp>;
    BookmarkImp() : BookmarkRepository() {}

    void add(UserSession &session) { 
        save(session, true);
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_USERSESSION_H