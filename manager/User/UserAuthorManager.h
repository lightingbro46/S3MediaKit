#ifndef MANAGER_USER_H
#define MANAGER_USER_H

#include "Common/MediaSource.h"
#include "Poller/Timer.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace managerkit {

/**
 * Authorization cache of user with device, default max elapsed is 60 seconds
 */
class UserAuthorCache : public toolkit::noncopyable {
public:
    using Ptr = std::shared_ptr<UserAuthorCache>;
    UserAuthorCache(std::string user_id = "", std::string device_id = "", bool permit = false, uint64_t max_elapsed = 60)
        : _uid(user_id)
        , _device_id(device_id)
        , _is_permit(permit) {
        _created_at = time(nullptr);
        _expired_at = _created_at + max_elapsed;
    }

    /**
     * get created at timestamp, unit: second
     */
    uint64_t getCreatedAt() { return _created_at; }

    /**
     * get expired at timestamp, unit: second
     */
    uint64_t getExpiredAt() { return _expired_at; }

    /**
     * user has permission to access device data
     */
    bool hasLicensed() { return _is_permit; }

private:
    std::string _uid;
    std::string _device_id;
    uint64_t _created_at;
    uint64_t _expired_at;
    bool _is_permit;
};

/**
 * Token Manager, used for token expiration management.
 */
class UserAuthorManager : public std::enable_shared_from_this<UserAuthorManager> {
public:
    using Ptr = std::shared_ptr<UserAuthorManager>;
    using UserAuthorInvoker = std::function<void(const std::string &errMsg)>;

    /**
     * Get singleton
     */
    static UserAuthorManager &Instance();
    ~UserAuthorManager();

    /**
     * find cache
     */
    UserAuthorCache::Ptr findCache(const std::string &uid, const std::string &deviceId);

    /**
     *  get cache
     */
    UserAuthorCache::Ptr getAuthCache(const mediakit::MediaInfo &args, const std::string &jwt_token);

    /**
     * Add cache
     */
    void addAuthCache(const mediakit::MediaInfo &args, const std::string &jwt_token, bool permit);

private:
    UserAuthorManager();

    /**
     * Timer to clear blocked
     *
     */
    void onManager();

    /**
     * Traver all user who has been blocked and release block if can
     *
     */
    void cleanExpiredCache();

    /**
     * verify JWT token
     *
     */
    bool verifyJwtToken(std::string &user_id, const std::string &jwt_token);

private:
    std::unordered_map<std::string /*uid*/, std::unordered_map<std::string /*deviceId*/, UserAuthorCache::Ptr>> _map_uid_cache;
    std::recursive_mutex _mtx;
    toolkit::Timer::Ptr _timer;
};

} // namespace managerkit

#endif // MANAGER_USER_H