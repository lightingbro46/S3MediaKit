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
class UserSessionCache : public toolkit::noncopyable {
public:
    using Ptr = std::shared_ptr<UserSessionCache>;
    UserSessionCache(const std::string &token);

    /**
     * get created at timestamp, unit: second
     */
    uint64_t getCreatedAt() { return _created_at; }

    /**
     * get expired at timestamp, unit: second
     */
    uint64_t getExpiredAt() { return _expired_at; }
    /**
     * get user id in cache
     */
    std::string getUid() { return _user_id; }

    /**
     *  get user name in cache
     */
    std::string getUserName() { return _user_name; }

    /**
     *  save user session into database
     */
    void saveUserSession();

private:
    std::string _user_id;
    std::string _user_name;
    uint64_t _created_at;
    uint64_t _expired_at;
    std::string _token;
};

enum class UserAuthorPermit : uint8_t {
    UNKNOWN = 0,
    ACCEPT = 1,
    REJECT= 2
};

/**
 * Token Manager, used for token expiration management.
 */
class UserAuthorManager : public std::enable_shared_from_this<UserAuthorManager> {
public:
    using Ptr = std::shared_ptr<UserAuthorManager>;

    /**
     * Get singleton
     */
    static UserAuthorManager &Instance();

    /**
     * Desconstructor
     */
    ~UserAuthorManager();

    /**
     *  get user-device author cache
     */
    UserSessionCache::Ptr getTokenCache(const std::string &jwt_token);

    /**
     *  get user-device author cache
     */
    UserAuthorPermit getAuthorCache(const mediakit::MediaInfo &args, const std::string &jwt_token);

    /**
     * Add cache
     */
    void addAuthorCache(const mediakit::MediaInfo &args, const std::string &jwt_token, bool permit);

private:
    /*
     * Constructor
     */
    UserAuthorManager(uint64_t max_elapsed = 300);

    /**
     * find user-device author cache
     */
    UserAuthorPermit findAuthorCache(const std::string &jwt_token, const std::string &device_id);

    /**
     * Timer to clear blocked
     */
    void onManager();

    /**
     * Traver all token-device author cache expired
     */
    void cleanExpiredAuthorCache();

    /**
     * Traver all token expired
     */
    void cleanExpiredTokenCache();

private:
    std::unordered_map<std::string /*token*/, UserSessionCache::Ptr> _map_token_cache;
    std::unordered_map<std::string /*token*/, std::unordered_map<std::string /*deviceId*/, std::pair<bool /*permit*/, uint64_t /*create_time*/>>> _map_token_device;
    std::recursive_mutex _mtx;
    toolkit::Timer::Ptr _timer;
    uint64_t _max_elapsed;
};

} // namespace managerkit

#endif // MANAGER_USER_H