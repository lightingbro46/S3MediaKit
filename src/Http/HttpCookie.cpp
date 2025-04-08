#include "HttpCookie.h"
#include "Util/util.h"
#include "Util/onceToken.h"

#if defined(_WIN32)
#include "Util/strptime_win.h"
#endif

using namespace toolkit;
using namespace std;

namespace mediakit {

void HttpCookie::setPath(const string &path) {
    _path = path;
}

void HttpCookie::setHost(const string &host) {
    _host = host;
}

// from https://gmbabar.wordpress.com/2010/12/01/mktime-slow-use-custom-function/#comment-58
static time_t time_to_epoch(const struct tm *ltm, int utcdiff) {
    const int mon_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    long tyears, tdays, leaps, utc_hrs;
    int i;

    tyears = ltm->tm_year - 70; // tm->tm_year is from 1900.
    leaps = (tyears + 2) / 4; // no of next two lines until year 2100.
    // i = (ltm->tm_year – 100) / 100;
    // leaps -= ( (i/4)*3 + i%4 );
    tdays = 0;
    for (i = 0; i < ltm->tm_mon; i++)
        tdays += mon_days[i];

    tdays += ltm->tm_mday - 1; // days of month passed.
    tdays = tdays + (tyears * 365) + leaps;

    utc_hrs = ltm->tm_hour + utcdiff; // for your time zone.
    return (tdays * 86400) + (utc_hrs * 3600) + (ltm->tm_min * 60) + ltm->tm_sec;
}

static time_t timeStrToInt(const string &date) {
    struct tm tt;
    strptime(date.data(), "%a, %b %d %Y %H:%M:%S %Z", &tt);
    // mktime uses mutex internally, which significantly affects performance
    return time_to_epoch(&tt, getGMTOff() / 3600); // mktime(&tt);
}

void HttpCookie::setExpires(const string &expires, const string &server_date) {
    _expire = timeStrToInt(expires);
    if (!server_date.empty()) {
        _expire = time(NULL) + (_expire - timeStrToInt(server_date));
    }
}

void HttpCookie::setKeyVal(const string &key, const string &val) {
    _key = key;
    _val = val;
}

HttpCookie::operator bool() {
    return !_host.empty() && !_key.empty() && !_val.empty() && (_expire > time(NULL));
}

const string &HttpCookie::getVal() const {
    return _val;
}

const string &HttpCookie::getKey() const {
    return _key;
}

HttpCookieStorage &HttpCookieStorage::Instance() {
    static HttpCookieStorage instance;
    return instance;
}

void HttpCookieStorage::set(const HttpCookie::Ptr &cookie) {
    lock_guard<mutex> lck(_mtx_cookie);
    if (!cookie || !(*cookie)) {
        return;
    }
    _all_cookie[cookie->_host][cookie->_path][cookie->_key] = cookie;
}

vector<HttpCookie::Ptr> HttpCookieStorage::get(const string &host, const string &path) {
    vector<HttpCookie::Ptr> ret(0);
    lock_guard<mutex> lck(_mtx_cookie);
    auto it = _all_cookie.find(host);
    if (it == _all_cookie.end()) {
        // No record found for this host
        return ret;
    }
    // Traverse all paths under this host
    for (auto &pr : it->second) {
        if (path.find(pr.first) != 0) {
            // This path does not match
            continue;
        }
        // Traverse all cookies under this path
        for (auto it_cookie = pr.second.begin(); it_cookie != pr.second.end();) {
            if (!*(it_cookie->second)) {
                // This cookie has expired, remove it
                it_cookie = pr.second.erase(it_cookie);
                continue;
            }
            // Save valid cookies
            ret.emplace_back(it_cookie->second);
            ++it_cookie;
        }
    }
    return ret;
}

} /* namespace mediakit */
