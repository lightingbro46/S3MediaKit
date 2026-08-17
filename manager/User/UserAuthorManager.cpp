#include "UserAuthorManager.h"
#include "Common/config.h"
#include "Network/Session.h"
#include "Util/logger.h"
#include "Util/NoticeCenter.h"
#include <algorithm>

using namespace std;
using namespace toolkit;
using namespace mediakit;

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
    {
        lock_guard<recursive_mutex> lck(_mtx);
        cleanExpiredAuthorCache();
        cleanExpiredTokenCache();
        cleanExpiredClusterAuthorCache();
    }
    checkActiveTokenAuthorizations();
}

void UserAuthorManager::registerMediaSession(const string &jwt_token, const string &device_id,
                                             const weak_ptr<toolkit::Session> &session) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto &sessions = _map_token_sessions[jwt_token];
    auto newSession = session.lock();
    for (auto &item : sessions) {
        auto existingSession = item.session.lock();
        if (item.device_id == device_id && existingSession && existingSession == newSession) {
            return;
        }
    }
    sessions.push_back({device_id, session});
}

void UserAuthorManager::checkActiveTokenAuthorizations() {
    vector<pair<string, string>> checks;
    const auto now = static_cast<uint64_t>(time(nullptr));
    {
        lock_guard<recursive_mutex> lck(_mtx);
        for (auto tokenIt = _map_token_sessions.begin(); tokenIt != _map_token_sessions.end(); ++tokenIt) {
            for (auto sessionIt = tokenIt->second.begin(); sessionIt != tokenIt->second.end();) {
                if (sessionIt->session.expired()) {
                    sessionIt = tokenIt->second.erase(sessionIt);
                    continue;
                }
                auto resourceIt = _map_token_resource.find(tokenIt->first);
                bool needsCheck = resourceIt == _map_token_resource.end();
                if (!needsCheck) {
                    auto deviceIt = resourceIt->second.find(sessionIt->device_id);
                    needsCheck = deviceIt == resourceIt->second.end() || deviceIt->second.second <= now;
                }
                if (needsCheck) {
                    checks.emplace_back(tokenIt->first, sessionIt->device_id);
                }
                ++sessionIt;
            }
        }
    }
    for (auto &item : checks) {
        checkTokenDevice(item.first, item.second);
    }
}

void UserAuthorManager::checkTokenDevice(const string &jwt_token, const string &device_id) {
    lock_guard<recursive_mutex> lck(_mtx);
    const string checkKey = jwt_token + ":" + device_id;
    if (_token_checks.count(checkKey)) {
        return;
    }
    auto tokenIt = _map_token_cache.find(jwt_token);
    if (tokenIt == _map_token_cache.end()) {
        revokeTokenDevice(jwt_token, device_id, "token cache unavailable");
        return;
    }
    if (tokenIt->second->getExpiredAt() <= static_cast<uint64_t>(time(nullptr))) {
        revokeToken(jwt_token, "access token expired");
        return;
    }
    _token_checks.insert(checkKey);

    Broadcast::AuthInvoker invoker = [jwt_token, device_id, checkKey](const string &err) {
        auto &manager = UserAuthorManager::Instance();
        lock_guard<recursive_mutex> lck(manager._mtx);
        manager._token_checks.erase(checkKey);
        if (!err.empty()) {
            manager.revokeTokenDevice(jwt_token, device_id, err);
            return;
        }
        const auto now = static_cast<uint64_t>(time(nullptr));
        auto tokenIt = manager._map_token_cache.find(jwt_token);
        if (tokenIt == manager._map_token_cache.end()) {
            return;
        }
        const auto expiredAt = tokenIt->second->getExpiredAt();
        const auto maxElapsed = tokenIt->second->getMaxElapsed();
        manager._map_token_resource[jwt_token][device_id] = make_pair(true, std::min(now + maxElapsed, expiredAt));
    };

    auto flag = NOTICE_EMIT(BroadcastDeviceAccessArgs, Broadcast::kBroadcastDeviceAccess, device_id, jwt_token, invoker);
    if (!flag) {
        invoker("No listener for device access authorization");
    }
}

void UserAuthorManager::revokeToken(const string &jwt_token, const string &reason) {
    auto sessionIt = _map_token_sessions.find(jwt_token);
    if (sessionIt != _map_token_sessions.end()) {
        for (auto &item : sessionIt->second) {
            if (auto session = item.session.lock()) {
                session->shutdown(SockException(Err_shutdown, "Session revoked: " + reason));
            }
        }
        _map_token_sessions.erase(sessionIt);
    }
    _map_token_resource.erase(jwt_token);
    _map_token_cache.erase(jwt_token);
    for (auto it = _token_checks.begin(); it != _token_checks.end();) {
        if (it->compare(0, jwt_token.size(), jwt_token) == 0) {
            it = _token_checks.erase(it);
        } else {
            ++it;
        }
    }
    WarnL << "Revoked media access token: " << reason;
}

void UserAuthorManager::revokeTokenDevice(const string &jwt_token, const string &device_id,
                                          const string &reason) {
    auto sessionIt = _map_token_sessions.find(jwt_token);
    if (sessionIt != _map_token_sessions.end()) {
        for (auto it = sessionIt->second.begin(); it != sessionIt->second.end();) {
            if (it->device_id != device_id) {
                ++it;
                continue;
            }
            if (auto session = it->session.lock()) {
                session->shutdown(SockException(Err_shutdown, "Session revoked: " + reason));
            }
            it = sessionIt->second.erase(it);
        }
        if (sessionIt->second.empty()) {
            _map_token_sessions.erase(sessionIt);
        }
    }

    auto resourceIt = _map_token_resource.find(jwt_token);
    if (resourceIt != _map_token_resource.end()) {
        resourceIt->second.erase(device_id);
        if (resourceIt->second.empty()) {
            _map_token_resource.erase(resourceIt);
        }
    }

    WarnL << "Revoked media access for device " << device_id << ": " << reason;
}

void UserAuthorManager::cleanExpiredAuthorCache() {
    const auto now = static_cast<uint64_t>(time(nullptr));
    // remove expired user-resource author cache
    for (auto tokenIt = _map_token_resource.begin(); tokenIt != _map_token_resource.end(); ++tokenIt) {
        auto &resourceMap = tokenIt->second;

        for (auto resourceIt = resourceMap.begin(); resourceIt != resourceMap.end();) {
            if (resourceIt->second.second < now) {
                resourceIt = resourceMap.erase(resourceIt);
            } else {
                ++resourceIt;
            }
        }
    }
}

void UserAuthorManager::cleanExpiredTokenCache() {
    const auto now = static_cast<uint64_t>(time(nullptr));
    // remove expired token cache
    for (auto tokenIt = _map_token_cache.begin(); tokenIt != _map_token_cache.end();) {
        if (tokenIt->second->getExpiredAt() < now) {
            auto sessionIt = _map_token_sessions.find(tokenIt->first);
            if (sessionIt != _map_token_sessions.end()) {
                for (auto it = sessionIt->second.begin(); it != sessionIt->second.end();) {
                    if (it->session.expired()) {
                        it = sessionIt->second.erase(it);
                    } else {
                        ++it;
                    }
                }
                // Keep active sessions until the periodic remote authorization
                // check decides whether the token must be revoked.
                if (!sessionIt->second.empty()) {
                    ++tokenIt;
                    continue;
                }
            }
            // remove all resource authorization related this token
            _map_token_resource.erase(tokenIt->first);
            _map_token_sessions.erase(tokenIt->first);
            tokenIt = _map_token_cache.erase(tokenIt);
        } else {
            ++tokenIt;
        }
    }
}

UserSessionCache::Ptr UserAuthorManager::getTokenCache(const string &jwt_token, const string &user_agent, const std::string &client_ip) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto tokenIt = _map_token_cache.find(jwt_token);
    return tokenIt != _map_token_cache.end() ? tokenIt->second : addTokenCache(jwt_token, user_agent, client_ip);
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
        auto resourceIt = tokenIt->second.find(resource_id);
        if (resourceIt != tokenIt->second.end()) {
            return resourceIt->second.first ? UserAuthorPermit::ACCEPT : UserAuthorPermit::REJECT;
        }
    }
    return UserAuthorPermit::UNKNOWN;
}

void UserAuthorManager::addAuthorCache(const string &resource_id, const string &jwt_token, bool permit, uint64_t max_elapsed) {
    lock_guard<recursive_mutex> lck(_mtx);

    const auto now = static_cast<uint64_t>(time(nullptr));
    auto tokenIt = _map_token_cache.find(jwt_token);
    const auto tokenExpiredAt = tokenIt == _map_token_cache.end() ? now + max_elapsed : tokenIt->second->getExpiredAt();
    _map_token_resource[jwt_token][resource_id] = std::make_pair(permit, std::min(now + max_elapsed, tokenExpiredAt));
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

    // Add cluster author cache with expiration time
    _map_cluster_author[author_id + ":" + key] = std::make_pair(permit, time(nullptr) + max_elapsed);
}

void UserAuthorManager::cleanExpiredClusterAuthorCache() {
    const auto now = static_cast<uint64_t>(time(nullptr));
    // remove expired cluster author cache
    for (auto authorIt = _map_cluster_author.begin(); authorIt != _map_cluster_author.end();) {
        if (authorIt->second.second < now) {
            authorIt = _map_cluster_author.erase(authorIt);
        } else {
            ++authorIt;
        }
    }
}

} // namespace managerkit
