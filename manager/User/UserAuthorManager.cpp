#include "UserAuthorManager.h"
#include "../../server/Manager.h"
#include "Common/config.h"
#include "Util/base64.h"
#include "Util/util.h"
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/traits.h>
#include "Storage/UserSession.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using traits = jwt::traits::open_source_parsers_jsoncpp;

namespace managerkit {

INSTANCE_IMP(UserAuthorManager);

UserAuthorManager::UserAuthorManager(uint64_t max_elapsed) : _max_elapsed(max_elapsed) {
    _timer = std::make_shared<Timer>(
        60.0f,
        [this]() {
            onManager();
            return true;
        },
        nullptr);
}

UserAuthorManager::~UserAuthorManager() {
    _timer.reset();
}

static bool verifyJwtToken(const string &jwt_token) {
    try {
        // step 1. parsing token
        std::string token = jwt_token;
        auto decoded = jwt::decode<traits>(token);

        // step 2. verify jwt token
        GET_CONFIG(string, public_key, Manager::kJwtPublicKey)
        if (public_key.empty()) {
            WarnL << "Public key empty. Ignore verify jwt token.";
            return false;
        }
        auto decoded_public_key = decodeBase64(public_key);
        jwt::verify<traits>().allow_algorithm(jwt::algorithm::rs256(decoded_public_key)).verify(decoded);

    } catch (exception &ex) {
        WarnL << "Verify jwt token failed: " << ex.what();
        return false;
    }

    return true;
};

static bool decodeJwtToken(Json::Value &decoded_payload, const string &jwt_token) {
    try {
        // step 1. parsing token
        std::string token = jwt_token;
        auto decoded = jwt::decode<traits>(token);
        // step 2. get decoded token, include user_id, sub
        auto payload = decoded.get_payload_json();
        decoded_payload = payload;
    } catch (exception &ex) {
        WarnL << "Decode jwt token failed: " << ex.what();
        return false;
    }

    return true;
};

void UserAuthorManager::onManager() {
    lock_guard<recursive_mutex> lck(_mtx);
    cleanExpiredAuthorCache();
    cleanExpiredTokenCache();
}

void UserAuthorManager::cleanExpiredAuthorCache() {
    auto time_threshold = time(nullptr) - _max_elapsed;

    // remove expired user-device author cache
    for (auto tokenIt = _map_token_device.begin(); tokenIt != _map_token_device.end(); ++tokenIt) {
        auto &deviceMap = tokenIt->second;

        for (auto devIt = deviceMap.begin(); devIt != deviceMap.end();) {
            auto &info = devIt->second;
            uint64_t create_time = info.second;
            if (create_time < time_threshold) {
                devIt = deviceMap.erase(devIt);
            } else {
                ++devIt;
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
            _map_token_device.erase(tokenIt->first);
            tokenIt = _map_token_cache.erase(tokenIt);
        } else {
            ++tokenIt;
        }
    }
}

UserAuthorPermit UserAuthorManager::findAuthorCache(const string &jwt_token, const string &device_id) {
    auto tokenIt = _map_token_device.find(jwt_token);
    if (tokenIt != _map_token_device.end()) {
        auto deviceMap = tokenIt->second;
        auto deviceIt = deviceMap.find(device_id);
        if (deviceIt != deviceMap.end()) {
            auto pair = deviceIt->second;
            return pair.first ? UserAuthorPermit::ACCEPT : UserAuthorPermit::REJECT;
        }
    }
    return UserAuthorPermit::UNKNOWN;
};

UserSessionCache::Ptr UserAuthorManager::getTokenCache(const string &jwt_token) {
    lock_guard<recursive_mutex> lck(_mtx);
    return _map_token_cache.find(jwt_token) != _map_token_cache.end() ? _map_token_cache[jwt_token] : nullptr;
}

UserAuthorPermit UserAuthorManager::getAuthorCache(const MediaInfo &arg, const string &jwt_token) {
    lock_guard<recursive_mutex> lck(_mtx);
    string device_id = arg.app;
    auto ret = findAuthorCache(jwt_token, device_id);
    if (ret != UserAuthorPermit::UNKNOWN) {
        return ret;
    }

    if (!verifyJwtToken(jwt_token)) {
        // invalid token or expired token
        return UserAuthorPermit::REJECT;
    }
    return UserAuthorPermit::UNKNOWN;
}

void UserAuthorManager::addAuthorCache(const MediaInfo &arg, const string &jwt_token, bool permit) {
    lock_guard<recursive_mutex> lck(_mtx);
    string device_id = arg.app;

    // add to user-device author cache map
    _map_token_device[jwt_token][device_id] = std::make_pair(permit, time(nullptr));

    if (_map_token_cache.find(jwt_token) == _map_token_cache.end()) {
        auto cache = std::make_shared<UserSessionCache>(jwt_token);
        cache->saveUserSession();

        // add to cache map
        _map_token_cache[jwt_token] = cache;
    }
}

UserSessionCache::UserSessionCache(const string &token) : _token(token) {
    Json::Value decoded_payload;
    if (!decodeJwtToken(decoded_payload, token)) {
        throw std::runtime_error("Invalid jwt token");
    }

    _user_id = decoded_payload["user_id"].asString();
    _user_name = decoded_payload["sub"].asString();
    _created_at = time(nullptr);
    _expired_at = decoded_payload["exp"].asUInt64();
}

void UserSessionCache::saveUserSession() {
    UserSession session;
    session.token = _token;
    session.userId = _user_id;
    session.creationTimeS = _created_at;
    auto imp_session = std::make_shared<UserSessionImp>();
    imp_session->add(session, _user_name);
}

} // namespace managerkit
