#include "UserAuthorManager.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

INSTANCE_IMP(UserAuthorManager);

UserAuthorManager::UserAuthorManager() {
    _timer = std::make_shared<Timer>(
        60.0f,
        []() {
            // Use singleton access in timer callback to avoid capturing a raw
            // pointer across thread boundaries.
            UserAuthorManager::Instance().onManager();
            return true;
        },
        nullptr);
}

UserAuthorManager::~UserAuthorManager() {
    _timer.reset();
}

void UserAuthorManager::onManager() {
    lock_guard<recursive_mutex> lck(_mtx);
    cleanExpiredAuthorCache();
    cleanExpiredTokenCache();
    cleanExpiredClusterAuthorCache();
}

void UserAuthorManager::cleanExpiredAuthorCache() {
    auto time_now = time(nullptr);
    // remove expired user-resource author cache
    for (auto tokenIt = _map_token_resource.begin(); tokenIt != _map_token_resource.end(); ++tokenIt) {
        auto &resourceMap = tokenIt->second;

        for (auto resourceIt = resourceMap.begin(); resourceIt != resourceMap.end();) {
            auto &info = resourceIt->second;
            uint64_t expired_time = info.second;
            if (expired_time < (uint64_t)time_now) {
                resourceIt = resourceMap.erase(resourceIt);
            } else {
                ++resourceIt;
            }
        }
    }
}

void UserAuthorManager::cleanExpiredTokenCache() {
    auto time_now = time(nullptr);
    // remove expired token cache
    for (auto tokenIt = _map_token_cache.begin(); tokenIt != _map_token_cache.end();) {
        auto tokenCache = tokenIt->second;
        uint64_t expired_time = tokenCache->getExpiredAt();
        if (expired_time < (uint64_t)time_now) {
            // remove all resource authorization related this token
            _map_token_resource.erase(tokenIt->first);
            tokenIt = _map_token_cache.erase(tokenIt);
        } else {
            ++tokenIt;
        }
    }
}

UserSessionCache::Ptr UserAuthorManager::getTokenCache(const string &jwt_token, const string &user_agent, const std::string &client_ip) {
    lock_guard<recursive_mutex> lck(_mtx);
    return _map_token_cache.find(jwt_token) != _map_token_cache.end() ? _map_token_cache[jwt_token] : addTokenCache(jwt_token, user_agent, client_ip);
}

UserSessionCache::Ptr UserAuthorManager::addTokenCache(const string &jwt_token, const string &user_agent, const std::string &client_ip) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto cache = std::make_shared<UserSessionCache>(jwt_token, user_agent, client_ip);
    // add to cache map
    _map_token_cache[jwt_token] = cache;
    return cache;
}

UserAuthorPermit UserAuthorManager::getAuthorCache(const string &resource_id, const string &jwt_token) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto tokenIt = _map_token_resource.find(jwt_token);
    if (tokenIt != _map_token_resource.end()) {
        auto &resourceMap = tokenIt->second;
        auto resourceIt = resourceMap.find(resource_id);
        if (resourceIt != resourceMap.end()) {
            auto &info = resourceIt->second;
            return info.first ? UserAuthorPermit::ACCEPT : UserAuthorPermit::REJECT;
        }
    }
    return UserAuthorPermit::UNKNOWN;
}

void UserAuthorManager::addAuthorCache(const string &resource_id, const string &jwt_token, bool permit, uint64_t max_elapsed) {
    lock_guard<recursive_mutex> lck(_mtx);

    uint64_t expired_time = time(nullptr) + max_elapsed;
    // add to user-device author cache map
    _map_token_resource[jwt_token][resource_id] = std::make_pair(permit, expired_time);
}

UserAuthorPermit UserAuthorManager::getClusterAuthorCache(const std::string &author_id, const std::string &secret_key) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto key = author_id + ":" + secret_key;
    auto authorIt = _map_cluster_author.find(key);
    if (authorIt != _map_cluster_author.end()) {
        auto &info = authorIt->second;
        return info.first ? UserAuthorPermit::ACCEPT : UserAuthorPermit::REJECT;
    }
    return UserAuthorPermit::UNKNOWN;
}

void UserAuthorManager::addClusterAuthorCache(const std::string &author_id, const std::string &key, bool permit, uint64_t max_elapsed) {
    lock_guard<recursive_mutex> lck(_mtx);

    uint64_t expired_time = time(nullptr) + max_elapsed;
    // add to cluster author cache map
    _map_cluster_author[author_id + ":" + key] = std::make_pair(permit, expired_time);
}

void UserAuthorManager::cleanExpiredClusterAuthorCache() {
    auto time_now = time(nullptr);
    // remove expired cluster author cache
    for (auto authorIt = _map_cluster_author.begin(); authorIt != _map_cluster_author.end();) {
        auto &info = authorIt->second;
        uint64_t expired_time = info.second;
        if (expired_time < (uint64_t)time_now) {
            authorIt = _map_cluster_author.erase(authorIt);
        } else {
            ++authorIt;
        }
    }
}

} // namespace managerkit
