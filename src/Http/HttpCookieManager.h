#ifndef SRC_HTTP_COOKIEMANAGER_H
#define SRC_HTTP_COOKIEMANAGER_H

#include "Common/Parser.h"
#include "Network/Socket.h"
#include "Util/TimeTicker.h"
#include "Util/mini.h"
#include "Util/util.h"
#include <memory>
#include <unordered_map>

#define COOKIE_DEFAULT_LIFE (7 * 24 * 60 * 60)

namespace mediakit {

class HttpCookieManager;

/**
 * cookie object, used to store some related attributes of the cookie
 */
class HttpServerCookie : public toolkit::noncopyable {
public:
    using Ptr = std::shared_ptr<HttpServerCookie>;
    /**
     * Construct cookie
     * @param manager cookie manager object
     * @param cookie_name cookie name, such as MY_SESSION
     * @param uid user unique id
     * @param cookie cookie random string
     * @param max_elapsed maximum expiration time, in seconds
     */

    HttpServerCookie(
        const std::shared_ptr<HttpCookieManager> &manager, const std::string &cookie_name, const std::string &uid,
        const std::string &cookie, uint64_t max_elapsed);
    ~HttpServerCookie();

    /**
     * Get uid
     * @return uid
     */
    const std::string &getUid() const;

    /**
     * Get the value of the Set-Cookie field in http
     * @param cookie_name the name of this cookie, such as MY_SESSION
     * @param path http access path
     * @return For example, MY_SESSION=XXXXXX;expires=Wed, Jun 12 2019 06:30:48 GMT;path=/index/files/
     */
    std::string getCookie(const std::string &path) const;

    /**
     * Get cookie random string
     * @return cookie random string
     */
    const std::string &getCookie() const;

    /**
     * Get the name of this cookie
     * @return
     */
    const std::string &getCookieName() const;

    /**
     * Update the expiration time of this cookie, so that this cookie will not expire
     */
    void updateTime();

    /**
     * Determine whether this cookie has expired
     * @return
     */
    bool isExpired();

    /**
     * Set additional data
     */
    void setAttach(toolkit::Any attach);

    /*
     * Get additional data
     */
    template <class T>
    T& getAttach() {
        return _attach.get<T>();
    }

private:
    std::string cookieExpireTime() const;

private:
    std::string _uid;
    std::string _cookie_name;
    std::string _cookie_uuid;
    uint64_t _max_elapsed;
    toolkit::Ticker _ticker;
    toolkit::Any _attach;
    std::weak_ptr<HttpCookieManager> _manager;
};

/**
 * cookie random string generator
 */
class RandStrGenerator {
public:

    /**
     * Get a random string that does not collide
     * @return random string
     */
    std::string obtain();

    /**
     * Release random string
     * @param str random string
     */
    void release(const std::string &str);

private:
    std::string obtain_l();

private:
    // Collision library
    std::unordered_set<std::string> _obtained;
    // Increase index, used to prevent collisions
    int _index = 0;
};

/**
 * Cookie manager, used to manage cookie generation and expiration management, and also implements the function of occupying login from different locations with the same account
 * This object implements the function that the same account can log in to at most several devices
 */
class HttpCookieManager : public std::enable_shared_from_this<HttpCookieManager> {
public:
    friend class HttpServerCookie;
    using Ptr =  std::shared_ptr<HttpCookieManager>;
    ~HttpCookieManager();

    /**
     *  Get singleton
     */
    static HttpCookieManager &Instance();

    /**
     * Add cookie
     * @param cookie_name cookie name, such as MY_SESSION
     * @param uid user id, if empty, it is anonymous login
     * @param max_client the maximum number of devices that this account can log in to
     * @param max_elapsed the expiration time of this cookie, in seconds
     * @return cookie object
     */
    HttpServerCookie::Ptr addCookie(
        const std::string &cookie_name, const std::string &uid, uint64_t max_elapsed = COOKIE_DEFAULT_LIFE,
        toolkit::Any = toolkit::Any{},
        int max_client = 1);

    /**
     * Find cookie object by cookie random string
     * @param cookie_name cookie name, such as MY_SESSION
     * @param cookie cookie random string
     * @return cookie object, can be nullptr
     */
    HttpServerCookie::Ptr getCookie(const std::string &cookie_name, const std::string &cookie);

    /**
     * Get cookie object from http header
     * @param cookie_name cookie name, such as MY_SESSION
     * @param http_header http header
     * @return cookie object
     */
    HttpServerCookie::Ptr getCookie(const std::string &cookie_name, const StrCaseMap &http_header);

    /**
     * Get cookie by uid
     * @param cookie_name cookie name, such as MY_SESSION
     * @param uid user id
     * @return cookie object
     */
    HttpServerCookie::Ptr getCookieByUid(const std::string &cookie_name, const std::string &uid);

    /**
     * Delete cookie, used when user logs out
     * @param cookie cookie object, can be nullptr
     * @return
     */
    bool delCookie(const HttpServerCookie::Ptr &cookie);

private:
    HttpCookieManager();

    void onManager();
    /**
     * Triggered when constructing a cookie object, the purpose is to record multiple cookies under a certain account
     * @param cookie_name cookie name, such as MY_SESSION
     * @param uid user id
     * @param cookie cookie random string
     */
    void onAddCookie(const std::string &cookie_name, const std::string &uid, const std::string &cookie);

    /**
     * Triggered when destructing a cookie object
     * @param cookie_name cookie name, such as MY_SESSION
     * @param uid user id
     * @param cookie cookie random string
     */
    void onDelCookie(const std::string &cookie_name, const std::string &uid, const std::string &cookie);

    /**
     * Get the cookie that logged in first under a certain username, the purpose is to implement the function that at most several devices can log in under a certain user
     * @param cookie_name cookie name, such as MY_SESSION
     * @param uid user id
     * @param max_client the maximum number of devices that can log in
     * @return the earliest cookie random string
     */
    std::string getOldestCookie(const std::string &cookie_name, const std::string &uid, int max_client = 1);

    /**
     * Delete cookie
     * @param cookie_name cookie name, such as MY_SESSION
     * @param cookie cookie random string
     * @return success true
     */
    bool delCookie(const std::string &cookie_name, const std::string &cookie);

private:
    std::unordered_map<
        std::string /*cookie_name*/, std::unordered_map<std::string /*cookie*/, HttpServerCookie::Ptr /*cookie_data*/>>
        _map_cookie;
    std::unordered_map<
        std::string /*cookie_name*/,
        std::unordered_map<std::string /*uid*/, std::map<uint64_t /*cookie time stamp*/, std::string /*cookie*/>>>
        _map_uid_to_cookie;
    std::recursive_mutex _mtx_cookie;
    toolkit::Timer::Ptr _timer;
    RandStrGenerator _generator;
};

} // namespace mediakit

#endif // SRC_HTTP_COOKIEMANAGER_H
