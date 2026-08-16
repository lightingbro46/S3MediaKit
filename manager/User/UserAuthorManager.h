#ifndef MANAGER_USERAUTHORMANAGER_H
#define MANAGER_USERAUTHORMANAGER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Poller/Timer.h"
#include "UserSessionCache.h"

namespace toolkit { class Session; }

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
    UserSessionCache::Ptr getTokenCache(const std::string &jwt_token, const std::string &user_agent = "", const std::string &client_ip = "");

    /** 
     * Register a media session authenticated with a token and device. 
     */
    void registerMediaSession(const std::string &jwt_token, const std::string &device_id,
                              const std::weak_ptr<toolkit::Session> &session);

    /**
     *  get user-resource author cache
     */
    UserAuthorPermit getAuthorCache(const std::string &resource_id, const std::string &jwt_token);

    /**
     * Add user-resource author cache
     */
    void addAuthorCache(const std::string &resource_id, const std::string &jwt_token, bool permit = false, uint64_t max_elapsed = 600);

    /**
     * Add cluster author cache
     */
    void addClusterAuthorCache(const std::string &author_id, const std::string &key, bool permit = false, uint64_t max_elapsed = 300);

    /**
     * Get cluster author cache
     */
    UserAuthorPermit getClusterAuthorCache(const std::string &author_id, const std::string &key);

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
    UserSessionCache::Ptr addTokenCache(const std::string &jwt_token, const std::string &user_agent, const std::string &client_ip);

    /**
     * Traver all token-resource author cache expired
     */
    void cleanExpiredAuthorCache();

    /**
     * Traver all token expired
     */
    void cleanExpiredTokenCache();

    /**
     * Traver all cluster author cache expired
     */
    void cleanExpiredClusterAuthorCache();

    /**
     * Traver all media session expired
     */
    void checkActiveTokenAuthorizations();

    /** 
     * Check authorization for one token/device pair. 
     * @param jwt_token The token to check.
     * @param device_id The device ID to check.
     * This function checks if the specified token/device pair is authorized to access resources.
     * If the pair is not authorized, it will be added to the list of checks to be performed. 
     * This function is intended to be called periodically to ensure that all active token/device pairs are still authorized.
     * It is thread-safe and can be called from multiple threads concurrently
     */
    void checkTokenDevice(const std::string &jwt_token, const std::string &device_id);

    /**
     * Revoke a token and all associated media sessions, and notify the user of the reason for revocation.
     * @param jwt_token The token to revoke.
     * @param reason The reason for revocation.
     * This function will remove the token from the cache, terminate any associated media sessions, 
     * and notify the user of the revocation reason. 
     * It is intended to be called when a token is deemed invalid or compromised, to ensure that the user 
     * is aware of the revocation and can take appropriate action
     */
    void revokeToken(const std::string &jwt_token, const std::string &reason);

    /**
     * Revoke a specific device session associated with a token, and notify the user of the reason for revocation.
     * @param jwt_token The token associated with the device session to revoke.
     * @param device_id The ID of the device session to revoke.
     * @param reason The reason for revocation.
     * This function will remove the specific device session from the cache, terminate the session,
     * and notify the user of the revocation reason.
     * It is intended to be called when a device session is deemed invalid or compromised, to ensure that the user is aware of the revocation and can take appropriate action.
     */
    void revokeTokenDevice(const std::string &jwt_token, const std::string &device_id,
                           const std::string &reason);

private:
    std::unordered_map<std::string /*token*/, UserSessionCache::Ptr> _map_token_cache;
    std::unordered_map<std::string /*token*/, std::unordered_map<std::string /*resourceId*/, std::pair<bool /*permit*/, uint64_t /*expired_time*/>>> _map_token_resource;
    std::unordered_map<std::string /*authorId + ":" + secretKey*/, std::pair<bool /*permit*/, uint64_t /*expired_time*/>> _map_cluster_author;
    struct MediaSessionInfo {
        std::string device_id;
        std::weak_ptr<toolkit::Session> session;
    };
    std::unordered_map<std::string /*token*/, std::vector<MediaSessionInfo>> _map_token_sessions;
    std::unordered_set<std::string /*token:device*/> _token_checks;
    std::recursive_mutex _mtx;
    toolkit::Timer::Ptr _timer;
};

} // namespace managerkit

#endif // MANAGER_USERAUTHORMANAGER_H                                                                                                                                                                                                                                                                                                                          