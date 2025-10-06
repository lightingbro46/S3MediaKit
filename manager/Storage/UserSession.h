#ifndef S3MANAGERKIT_USERSESSION_H
#define S3MANAGERKIT_USERSESSION_H

#include "DbStorage.h"
#include "Util/util.h"
#include "UserEntity.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

struct UserSession {
    std::string token;
    std::string userId;
    int64_t creationTimeS;
};

DECLARE_ENTITY(UserSession, "user_sessions", 
    { "token" },
    &UserSession::token, "token",
    &UserSession::userId, "userId",
    &UserSession::creationTimeS, "creationTimeS"
)

class UserSessionRepository : public SqliteRepository<UserSession> {
public:
    UserSessionRepository() : SqliteRepository<UserSession>(Database::kMediaServerDb) {}

protected:
    std::vector<UserSession> findByCreateTime(uint64_t stamp) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << " creationTimeS < ? ";
        whereParams.push_back(std::to_string(stamp));

        auto query = toolkit::QueryBuilder()
                            .select(EntityTraits<UserSession>::getColumns())
                            .from(EntityTraits<UserSession>::tableName())
                            .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<UserSession> ret;
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<UserSession>::fromRow(row));
        }
        return ret;
    }

    bool removeByCreateTime(uint64_t stamp) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << " creationTimeS < ? ";
        whereParams.push_back(std::to_string(stamp));

        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<UserSession>::tableName())
                            .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }
};

class UserSessionImp : public UserSessionRepository {
public:
    using Ptr = std::shared_ptr<UserSessionImp>;
    UserSessionImp() : UserSessionRepository() {}

    void add(UserSession &session, const std::string &username) {
        auto sessions = findById(session.token);
        if (sessions.size() == 0) {
            save(session, true);

            if (!username.empty()) {
                UserEntity user;
                user.userId = session.userId;
                user.userName = username;
                auto imp = std::make_shared<UserEntityImp>();
                imp->add(user);
            }
        }
    }

    // delete
    void remove(const std::string &token) {
        UserSession s;
        s.token = token;
        removeById(s);
    }

    // find
    std::vector<UserSession> findById(std::string id) {
        UserSession session_search;
        session_search.token = id;
        return UserSessionRepository::findById(session_search);
    }

    bool removeByStamp(uint64_t stamp) {
        auto ret = findByCreateTime(stamp);
        if (!ret.size()) {
            return true;
        }
        return removeByCreateTime(stamp);
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_USERSESSION_H