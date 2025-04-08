#ifndef Http_HttpClient_h
#define Http_HttpClient_h

#include <stdio.h>
#include <string.h>
#include <functional>
#include <memory>
#include "Util/util.h"
#include "Util/mini.h"
#include "Network/TcpClient.h"
#include "Common/Parser.h"
#include "HttpRequestSplitter.h"
#include "HttpCookie.h"
#include "HttpChunkedSplitter.h"
#include "Common/strCoding.h"
#include "HttpBody.h"

namespace mediakit {

class HttpArgs : public std::map<std::string, toolkit::variant, StrCaseCompare> {
public:
    std::string make() const {
        std::string ret;
        for (auto &pr : *this) {
            ret.append(pr.first);
            ret.append("=");
            ret.append(strCoding::UrlEncodeComponent(pr.second));
            ret.append("&");
        }
        if (ret.size()) {
            ret.pop_back();
        }
        return ret;
    }
};

class HttpClient : public toolkit::TcpClient, public HttpRequestSplitter {
public:
    using HttpHeader = StrCaseMap;
    using Ptr = std::shared_ptr<HttpClient>;

    /**
     * Send http[s] request
     * @param url Request url
     */
    virtual void sendRequest(const std::string &url);

    /**
     * Reset object
     */
    virtual void clear();

    /**
     * Set http method
     * @param method GET/POST etc.
     */
    void setMethod(std::string method);

    /**
     * Override http header
     * @param header
     */
    void setHeader(HttpHeader header);

    HttpClient &addHeader(std::string key, std::string val, bool force = false);

    /**
     * Set http content
     * @param body http content
     */
    void setBody(std::string body);

    /**
     * Set http content
     * @param body http content
     */
    void setBody(HttpBody::Ptr body);

    /**
     * Get response, valid after receiving the complete response
     */
    const Parser &response() const;

    /**
     * Get the body size declared in the response header
     */
    ssize_t responseBodyTotalSize() const;

    /**
     * Get the size of the body that has been downloaded
     */
    size_t responseBodySize() const;

    /**
     * Get the request url
     */
    const std::string &getUrl() const;

    /**
     * Determine if the response is pending
     */
    bool waitResponse() const;

    /**
     * Determine if it is https
     */
    bool isHttps() const;

    /**
     * Set the delay from initiating the connection to receiving the header, default 10 seconds
     * This parameter must be greater than 0
     */
    void setHeaderTimeout(size_t timeout_ms);

    /**
     * Set the timeout for receiving body data, default 5 seconds
     * This parameter can be used to handle timeout issues for large body responses
     * This parameter can be equal to 0
     */
    void setBodyTimeout(size_t timeout_ms);

    /**
     * Set the timeout for the entire link, default 0
     * After this value is set to non-zero, HeaderTimeout and BodyTimeout are invalid
     */
    void setCompleteTimeout(size_t timeout_ms);

    /**
     * Set http proxy url
     */
    void setProxyUrl(std::string proxy_url);

    /**
     * When the reuse connection fails, whether to allow the request to be resent
     * @param allow true: allow the request to be resent
     */
    void setAllowResendRequest(bool allow);

protected:
    /**
     * Receive http response header
     * @param status Status code, such as: 200 OK
     * @param headers http header
     */
    virtual void onResponseHeader(const std::string &status, const HttpHeader &headers) = 0;

    /**
     * Receive http content data
     * @param buf Data pointer
     * @param size Data size
     */
    virtual void onResponseBody(const char *buf, size_t size) = 0;

    /**
     * Receive http response complete,
     */
    virtual void onResponseCompleted(const toolkit::SockException &ex) = 0;

    /**
     * Redirect event
     * @param url Redirect url
     * @param temporary Whether it is a temporary redirect
     * @return Whether to continue
     */
    virtual bool onRedirectUrl(const std::string &url, bool temporary) { return true; };

protected:
    //// HttpRequestSplitter override ////
    ssize_t onRecvHeader(const char *data, size_t len) override;
    void onRecvContent(const char *data, size_t len) override;

    //// TcpClient override ////
    void onConnect(const toolkit::SockException &ex) override;
    void onRecv(const toolkit::Buffer::Ptr &pBuf) override;
    void onError(const toolkit::SockException &ex) override;
    void onFlush() override;
    void onManager() override;

    void clearResponse();

    bool checkProxyConnected(const char *data, size_t len);
    bool isUsedProxy() const;
    bool isProxyConnected() const;

private:
    void onResponseCompleted_l(const toolkit::SockException &ex);
    void onConnect_l(const toolkit::SockException &ex);
    void checkCookie(HttpHeader &headers);

private:
    //for http response
    bool _complete = false;
    bool _header_recved = false;
    bool _http_persistent = true;
    bool _allow_resend_request = false;
    size_t _recved_body_size;
    ssize_t _total_body_size;
    Parser _parser;
    std::shared_ptr<HttpChunkedSplitter> _chunked_splitter;

    //for request args
    bool _is_https;
    std::string _url;
    HttpHeader _user_set_header;
    HttpBody::Ptr _body;
    std::string _method;
    std::string _last_host;

    //for this request
    std::string _path;
    HttpHeader _header;

    //for timeout
    size_t _wait_header_ms = 10 * 1000;
    size_t _wait_body_ms = 10 * 1000;
    size_t _wait_complete_ms = 0;
    toolkit::Ticker _wait_header;
    toolkit::Ticker _wait_body;
    toolkit::Ticker _wait_complete;

    bool _used_proxy = false;
    bool _proxy_connected = false;
    uint16_t _proxy_port;
    std::string _proxy_url;
    std::string _proxy_host;
    std::string _proxy_auth;
};

} /* namespace mediakit */

#endif /* Http_HttpClient_h */
