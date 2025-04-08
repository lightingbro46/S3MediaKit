#include "HttpCookieManager.h"
#include "Common/config.h"
#include "Util/MD5.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

//////////////////////////////HttpServerCookie////////////////////////////////////
HttpServerCookie::HttpServerCookie(
    const std::shared_ptr<HttpCookieManager> &manager, const string &cookie_name, const string &uid,
    const string &cookie, uint64_t max_elapsed) {
    _uid = uid;
    _max_elapsed = max_elapsed;
    _cookie_uuid = cookie;
    _cookie_name = cookie_name;
    _manager = manager;
    manager->onAddCookie(_cookie_name, _uid, _cookie_uuid);
}

HttpServerCookie::~HttpServerCookie() {
    auto strongManager = _manager.lock();
    if (strongManager) {
        strongManager->onDelCookie(_cookie_name, _uid, _cookie_uuid);
    }
}

const string &HttpServerCookie::getUid() const {
    return _uid;
}

string HttpServerCookie::getCookie(const string &path) const {
    return (StrPrinter << _cookie_name << "=" << _cookie_uuid << ";expires=" << cookieExpireTime() << ";path=" << path);
}

const string &HttpServerCookie::getCookie() const {
    return _cookie_uuid;
}

const string &HttpServerCookie::getCookieName() const {
    return _cookie_name;
}

void HttpServerCookie::updateTime() {
    _ticker.resetTime();
}

bool HttpServerCookie::isExpired() {
    return _ticker.elapsedTime() > _max_elapsed * 1000;
}

void HttpServerCookie::setAttach(toolkit::Any attach) {
    _attach = std::move(attach);
}

string HttpServerCookie::cookieExpireTime() const {
    char buf[64];
    time_t tt = time(nullptr) + _max_elapsed;
    strftime(buf, sizeof buf, "%a, %b %d %Y %H:%M:%S GMT", gmtime(&tt));
    return buf;
}
//////////////////////////////CookieManager////////////////////////////////////
INSTANCE_IMP(HttpCookieManager);

HttpCookieManager::HttpCookieManager() {
    // Delete expired cookies periodically to prevent memory bloat
    _timer = std::make_shared<Timer>(
        10.0f,
        [this]() {
            onManager();
            return true;
        },
        nullptr);
}

HttpCookieManager::~HttpCookieManager() {
    _timer.reset();
}

void HttpCookieManager::onManager() {
    lock_guard<recursive_mutex> lck(_mtx_cookie);
    // First iterate through all types
    for (auto it_name = _map_cookie.begin(); it_name != _map_cookie.end();) {
        // Then iterate through all cookies under that type
        for (auto it_cookie = it_name->second.begin(); it_cookie != it_name->second.end();) {
            if (it_cookie->second->isExpired()) {
                // Cookie expired, remove record
                DebugL << it_cookie->second->getUid() << " Cookies expired:" << it_cookie->second->getCookie();
                it_cookie = it_name->second.erase(it_cookie);
                continue;
            }
            ++it_cookie;
        }

        if (it_name->second.empty()) {
            // There are no cookie records under this type, remove it
            DebugL << "There is no cookie record under this path:" << it_name->first;
            it_name = _map_cookie.erase(it_name);
            continue;
        }
        ++it_name;
    }
}

HttpServerCookie::Ptr HttpCookieManager::addCookie(const string &cookie_name, const string &uid_in, uint64_t max_elapsed, toolkit::Any attach, int max_client) {
    lock_guard<recursive_mutex> lck(_mtx_cookie);
    auto cookie = _generator.obtain();
    auto uid = uid_in.empty() ? cookie : uid_in;
    auto oldCookie = getOldestCookie(cookie_name, uid, max_client);
    if (!oldCookie.empty()) {
        // If the account has already logged in, delete the old cookie.
        // The purpose is to achieve login squeeze when multiple devices log in with the same account
        delCookie(cookie_name, oldCookie);
    }
    HttpServerCookie::Ptr data(new HttpServerCookie(shared_from_this(), cookie_name, uid, cookie, max_elapsed));
    data->setAttach(std::move(attach));
    // Save the new cookie under this account
    _map_cookie[cookie_name][cookie] = data;
    return data;
}

HttpServerCookie::Ptr HttpCookieManager::getCookie(const string &cookie_name, const string &cookie) {
    lock_guard<recursive_mutex> lck(_mtx_cookie);
    auto it_name = _map_cookie.find(cookie_name);
    if (it_name == _map_cookie.end()) {
        // There is no cookie of this type
        return nullptr;
    }
    auto it_cookie = it_name->second.find(cookie);
    if (it_cookie == it_name->second.end()) {
        // There is no corresponding cookie under this type
        return nullptr;
    }
    if (it_cookie->second->isExpired()) {
        // Cookie expired
        DebugL << "Cookies expired:" << it_cookie->second->getCookie();
        it_name->second.erase(it_cookie);
        return nullptr;
    }
    return it_cookie->second;
}

HttpServerCookie::Ptr HttpCookieManager::getCookie(const string &cookie_name, const StrCaseMap &http_header) {
    auto it = http_header.find("Cookie");
    if (it == http_header.end()) {
        return nullptr;
    }
    auto cookie = findSubString(it->second.data(), (cookie_name + "=").data(), ";");
    if (cookie.empty()) {
        cookie = findSubString(it->second.data(), (cookie_name + "=").data(), nullptr);
    }
    if (cookie.empty()) {
        return nullptr;
    }
    return getCookie(cookie_name, cookie);
}

HttpServerCookie::Ptr HttpCookieManager::getCookieByUid(const string &cookie_name, const string &uid) {
    if (cookie_name.empty() || uid.empty()) {
        return nullptr;
    }
    auto cookie = getOldestCookie(cookie_name, uid);
    if (cookie.empty()) {
        return nullptr;
    }
    return getCookie(cookie_name, cookie);
}

bool HttpCookieManager::delCookie(const HttpServerCookie::Ptr &cookie) {
    if (!cookie) {
        return false;
    }
    return delCookie(cookie->getCookieName(), cookie->getCookie());
}

bool HttpCookieManager::delCookie(const string &cookie_name, const string &cookie) {
    lock_guard<recursive_mutex> lck(_mtx_cookie);
    auto it_name = _map_cookie.find(cookie_name);
    if (it_name == _map_cookie.end()) {
        return false;
    }
    return it_name->second.erase(cookie);
}

void HttpCookieManager::onAddCookie(const string &cookie_name, const string &uid, const string &cookie) {
    // Add a new cookie, we record which cookies are under this uid, the purpose is to achieve login squeeze when multiple devices log in with the same account
    lock_guard<recursive_mutex> lck(_mtx_cookie);
    // Multiple cookies can exist under the same user (meaning multiple devices log in), these cookies are sorted in order of login time
    _map_uid_to_cookie[cookie_name][uid][getCurrentMillisecond()] = cookie;
}

void HttpCookieManager::onDelCookie(const string &cookie_name, const string &uid, const string &cookie) {
    lock_guard<recursive_mutex> lck(_mtx_cookie);
    // Recycle random string
    _generator.release(cookie);

    auto it_name = _map_uid_to_cookie.find(cookie_name);
    if (it_name == _map_uid_to_cookie.end()) {
        // No user has logged in under this type
        return;
    }
    auto it_uid = it_name->second.find(uid);
    if (it_uid == it_name->second.end()) {
        // This user has not logged in yet
        return;
    }

    // Iterate through all clients under the same user and remove the matching client
    for (auto it_cookie = it_uid->second.begin(); it_cookie != it_uid->second.end(); ++it_cookie) {
        if (it_cookie->second != cookie) {
            // Not this cookie
            continue;
        }
        // Remove a cookie under this username, this device cookie will become invalid
        it_uid->second.erase(it_cookie);

        if (!it_uid->second.empty()) {
            break;
        }

        // There are no devices online under this username, remove it
        it_name->second.erase(it_uid);

        if (!it_name->second.empty()) {
            break;
        }
        // There are no users online under this type, remove it
        _map_uid_to_cookie.erase(it_name);
        break;
    }
}

string HttpCookieManager::getOldestCookie(const string &cookie_name, const string &uid, int max_client) {
    lock_guard<recursive_mutex> lck(_mtx_cookie);
    auto it_name = _map_uid_to_cookie.find(cookie_name);
    if (it_name == _map_uid_to_cookie.end()) {
        // There is no cookie of this type
        return "";
    }
    auto it_uid = it_name->second.find(uid);
    if (it_uid == it_name->second.end()) {
        // This user has never logged in
        return "";
    }
    if ((int)it_uid->second.size() < MAX(1, max_client)) {
        // Under the same user, the number of clients has not reached the limit
        return "";
    }
    // The number of clients exceeds the limit, remove the first client to log in
    return it_uid->second.begin()->second;
}

/////////////////////////////////RandStrGenerator////////////////////////////////////
string RandStrGenerator::obtain() {
    // Get a unique anti-bloating random string
    while (true) {
        auto str = obtain_l();
        if (_obtained.find(str) == _obtained.end()) {
            // No duplicates
            _obtained.emplace(str);
            return str;
        }
    }
}

void RandStrGenerator::release(const string &str) {
    // Remove from the anti-bloating library
    _obtained.erase(str);
}

string RandStrGenerator::obtain_l() {
    // 12 pseudo-random bytes + 4 incrementing integer bytes, then md5 is the random string
    auto str = makeRandStr(12, false);
    str.append((char *)&_index, sizeof(_index));
    ++_index;
    return MD5(str).hexdigest();
}

} // namespace mediakit