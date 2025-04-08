#ifndef S3MEDIAKIT_PARSER_H
#define S3MEDIAKIT_PARSER_H

#include <map>
#include <string>
#include "Util/util.h"

namespace mediakit {

// Extract substring from string
std::string findSubString(const char *buf, const char *start, const char *end, size_t buf_size = 0);
// Parse url to host address and port number, compatible with ipv4/ipv6/dns
void splitUrl(const std::string &url, std::string &host, uint16_t &port);
// Parse proxy url, only supports http
void parseProxyUrl(const std::string &proxy_url, std::string &proxy_host, uint16_t &proxy_port, std::string &proxy_auth);

struct StrCaseCompare {
    bool operator()(const std::string &__x, const std::string &__y) const { return strcasecmp(__x.data(), __y.data()) < 0; }
};

class StrCaseMap : public std::multimap<std::string, std::string, StrCaseCompare> {
public:
    using Super = std::multimap<std::string, std::string, StrCaseCompare>;

    std::string &operator[](const std::string &k) {
        auto it = find(k);
        if (it == end()) {
            it = Super::emplace(k, "");
        }
        return it->second;
    }

    template <typename K, typename V>
    void emplace(K &&k, V &&v) {
        auto it = find(k);
        if (it != end()) {
            return;
        }
        Super::emplace(std::forward<K>(k), std::forward<V>(v));
    }

    template <typename K, typename V>
    void emplace_force(K &&k, V &&v) {
        Super::emplace(std::forward<K>(k), std::forward<V>(v));
    }
};

// rtsp/http/sip parsing class
class Parser {
public:
    // Parse http/rtsp/sip request, ensure buf ends with \0
    void parse(const char *buf, size_t size);

    // Get command word, such as GET/POST
    const std::string &method() const;

    // When requesting, get the middle url, excluding the parameters after ?
    const std::string &url() const;
    // When replying, get the status code, such as 200/404
    const std::string &status() const;

    // Get the middle url, including the parameters after ?
    std::string fullUrl() const;

    // When requesting, get the protocol name, such as HTTP/1.1
    const std::string &protocol() const;
    // When replying, get the status string, such as OK/Not Found
    const std::string &statusStr() const;

    // Get the request header value according to the header key name
    const std::string &operator[](const char *name) const;

    // Get http body or sdp
    const std::string &content() const;

    // Clear, for reuse
    void clear();

    // Get the parameters after ?
    const std::string &params() const;

    // Reset url
    void setUrl(std::string url);

    // Reset content
    void setContent(std::string content);

    // Get header list
    StrCaseMap &getHeader() const;

    // Get url parameter list
    StrCaseMap &getUrlArgs() const;

    // Parse the parameters after ?
    static StrCaseMap parseArgs(const std::string &str, const char *pair_delim = "&", const char *key_delim = "=");

    static std::string mergeUrl(const std::string &base_url, const std::string &path);

private:
    std::string _method;
    std::string _url;
    std::string _protocol;
    std::string _content;
    std::string _params;
    mutable StrCaseMap _headers;
    mutable StrCaseMap _url_args;
};

// Utility class for parsing rtsp url
class RtspUrl {
public:
    bool _is_ssl;
    uint16_t _port;
    std::string _url;
    std::string _user;
    std::string _passwd;
    std::string _host;

public:
    void parse(const std::string &url);

private:
    void setup(bool, const std::string &, const std::string &, const std::string &);
};

} // namespace mediakit

#endif // S3MEDIAKIT_PARSER_H
