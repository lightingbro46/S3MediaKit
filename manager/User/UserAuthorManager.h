#ifndef MANAGER_USERAUTHORMANAGER_H
#define MANAGER_USERAUTHORMANAGER_H

#include <memory>
#include <string>
#include <unordered_map>
#include "Poller/Timer.h"
#include "UserSessionCache.h"

namespace managerkit {

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
     *  get user session cache
     */
    UserSessionCache::Ptr getTokenCache(const std::string &jwt_token);

    /**
     *  get user-resource author cache
     */
    UserAuthorPermit getAuthorCache(const std::string &resource_id, const std::string &jwt_token);

    /**
     * Add user-resource author cache
     */
    void addAuthorCache(const std::string &resource_id, const std::string &jwt_token, bool permit = false, uint64_t max_elapsed = 600);

private:
    /*
     * Constructor
     */
    UserAuthorManager();

    /**
     * Timer to clear blocked
     */
    void onManager();

    /**
     *  add user session cache
     */
    UserSessionCache::Ptr addTokenCache(const std::string &jwt_token);

    /**
     * Traver all token-resource author cache expired
     */
    void cleanExpiredAuthorCache();

    /**
     * Traver all token expired
     */
    void cleanExpiredTokenCache();

private:
    std::unordered_map<std::string /*token*/, UserSessionCache::Ptr> _map_token_cache;
    std::unordered_map<std::string /*token*/, std::unordered_map<std::string /*resourceId*/, std::pair<bool /*permit*/, uint64_t /*expired_time*/>>> _map_token_resource;
    std::recursive_mutex _mtx;
    toolkit::Timer::Ptr _timer;
};

} // namespace managerkit

#endif // MANAGER_USERAUTHORMANAGER_H                                                                                                                                                                                                                                                                                                                          