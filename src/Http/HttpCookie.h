#ifndef S3MEDIAKIT_HTTPCOOKIE_H
#define S3MEDIAKIT_HTTPCOOKIE_H

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>

namespace mediakit {

/**
 * http client cookie object
 */
class HttpCookie {
public:
    using Ptr = std::shared_ptr<HttpCookie>;
    friend class HttpCookieStorage;

    void setPath(const std::string &path);
    void setHost(const std::string &host);
    void setExpires(const std::string &expires,const std::string &server_date);
    void setKeyVal(const std::string &key,const std::string &val);
    operator bool ();

    const std::string &getKey() const ;
    const std::string &getVal() const ;
private:
    std::string _host;
    std::string _path = "/";
    std::string _key;
    std::string _val;
    time_t _expire = 0;
};


/**
 * http client cookie global saver
 */
class HttpCookieStorage{
public:
    static HttpCookieStorage &Instance();
    void set(const HttpCookie::Ptr &cookie);
    std::vector<HttpCookie::Ptr> get(const std::string &host,const std::string &path);

private:
    HttpCookieStorage() = default;

private:
    std::unordered_map<std::string/*host*/, std::map<std::string/*cookie path*/,std::map<std::string/*cookie_key*/, HttpCookie::Ptr> > > _all_cookie;
    std::mutex _mtx_cookie;
};


} /* namespace mediakit */

#endif //S3MEDIAKIT_HTTPCOOKIE_H
