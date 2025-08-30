#include "UserAuthorManager.h"
#include "../../server/Manager.h"
#include "Common/config.h"
#include "Util/base64.h"
#include "Util/util.h"
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/traits.h>

using namespace std;
using namespace toolkit;
using namespace mediakit;
using traits = jwt::traits::open_source_parsers_jsoncpp;

namespace managerkit {

INSTANCE_IMP(UserAuthorManager);

UserAuthorManager::UserAuthorManager() {

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

bool UserAuthorManager::verifyJwtToken(string &user_id, const string &jwt_token) {
    lock_guard<recursive_mutex> lck(_mtx);

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
        jwt::verify<traits>().allow_algorithm(jwt::algorithm::rs256(public_key)).verify(decoded);

        // step 3. get user id
        auto payload = decoded.get_payload_json();
        user_id = payload["user_id"].asString();
    } catch (exception &ex) {
        WarnL << "Verify jwt token failed: " << ex.what();
        return false;
    }

    return true;
};

void UserAuthorManager::onManager() {
    // duyệt xem có user nào hết hạn bị block ko
    cleanExpiredCache();
}

void UserAuthorManager::cleanExpiredCache() {
    lock_guard<recursive_mutex> lck(_mtx);
    auto now = time(nullptr);

    // duyệt xem user nào hết hạn thì xóa khỏi cache
    for (auto userIt = _map_uid_cache.begin(); userIt != _map_uid_cache.end();) {
        auto &deviceMap = userIt->second;

        for (auto devIt = deviceMap.begin(); devIt != deviceMap.end();) {
            auto &info = devIt->second;
            time_t exp = info->getExpiredAt(); // không cần bộ đếm
            if (now > exp) {
                devIt = deviceMap.erase(devIt);
            } else {
                ++devIt;
            }
        }

        if (deviceMap.empty()) {
            userIt = _map_uid_cache.erase(userIt);
        } else {
            ++userIt;
        }
    }
}

UserAuthorCache::Ptr UserAuthorManager::findCache(const std::string &uid, const std::string &deviceId) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto userIt = _map_uid_cache.find(uid);

    if (userIt != _map_uid_cache.end()) {
        auto &deviceMap = userIt->second;
        auto devIt = deviceMap.find(deviceId);

        if (devIt != deviceMap.end()) {
            return devIt->second;
        }
    }
    return nullptr;
};

UserAuthorCache::Ptr UserAuthorManager::getAuthCache(const MediaInfo &arg, const string &jwt_token) {
    string device_id = arg.app;
    string user_id;
    if (!verifyJwtToken(user_id, jwt_token)) {
        // invalid token or expired token
        return std::make_shared<UserAuthorCache>();
    }

    TraceL << "Get user author cache: user " << user_id << "and device " << device_id;
    auto cache = findCache(user_id, device_id);

    return cache;
}

void UserAuthorManager::addAuthCache(const MediaInfo &arg, const string &jwt_token, bool permit) {
    lock_guard<recursive_mutex> lck(_mtx);
    std::string user_id;
    std::string device_id = arg.app;
    try {
        std::string token = jwt_token;
        auto decoded = jwt::decode<traits>(token);
        auto payload = decoded.get_payload_json();
        user_id = payload["user_id"].asString();
    } catch (exception &ex) {
        WarnL << " parsing user_id from jwt_token failed: " << ex.what();
        return;
    }

    // get user id
    auto cache = std::make_shared<UserAuthorCache>(user_id, device_id, permit);

    // add to cache
    _map_uid_cache[user_id][device_id] = cache;
}

} // namespace managerkit
