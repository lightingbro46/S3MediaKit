#include <cstdlib>
#include "Util/base64.h"
#include "HttpClient.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

void HttpClient::sendRequest(const string &url) {
    clearResponse();
    _url = url;
    auto protocol = findSubString(url.data(), NULL, "://");
    uint16_t port;
    bool is_https;
    if (strcasecmp(protocol.data(), "http") == 0) {
        port = 80;
        is_https = false;
    } else if (strcasecmp(protocol.data(), "https") == 0) {
        port = 443;
        is_https = true;
    } else {
        auto strErr = StrPrinter << "Illegal http url:" << url << endl;
        throw std::invalid_argument(strErr);
    }

    auto host = findSubString(url.data(), "://", "/");
    if (host.empty()) {
        host = findSubString(url.data(), "://", NULL);
    }
    _path = findSubString(url.data(), host.data(), NULL);
    if (_path.empty()) {
        _path = "/";
    }
    // Reset the header to prevent interference from the previous request's header
    _header = _user_set_header;
    auto pos = host.find('@');
    if (pos != string::npos) {
        // Remove the string after the "?"
        auto authStr = host.substr(0, pos);
        host = host.substr(pos + 1, host.size());
        _header.emplace("Authorization", "Basic " + encodeBase64(authStr));
    }
    auto host_header = host;
    splitUrl(host, host, port);
    _header.emplace("Host", host_header);
    _header.emplace("User-Agent", kServerName);
    _header.emplace("Accept", "*/*");
    _header.emplace("Accept-Language", "zh-CN,zh;q=0.8");
    if (_http_persistent) {
        _header.emplace("Connection", "keep-alive");
    } else {
        _header.emplace("Connection", "close");
    }
    _http_persistent = true;
    if (_body && _body->remainSize()) {
        _header.emplace("Content-Length", to_string(_body->remainSize()));
        GET_CONFIG(string, charSet, Http::kCharSet);
        _header.emplace("Content-Type", "application/x-www-form-urlencoded; charset=" + charSet);
    }

    bool host_changed = (_last_host != host + ":" + to_string(port)) || (_is_https != is_https);
    _last_host = host + ":" + to_string(port);
    _is_https = is_https;

    auto cookies = HttpCookieStorage::Instance().get(_last_host, _path);
    _StrPrinter printer;
    for (auto &cookie : cookies) {
        printer << cookie->getKey() << "=" << cookie->getVal() << ";";
    }
    if (!printer.empty()) {
        printer.pop_back();
        _header.emplace("Cookie", printer);
    }
    if (!alive() || host_changed || !_http_persistent) {
        if (isUsedProxy()) {
            _proxy_connected = false;
            startConnect(_proxy_host, _proxy_port, _wait_header_ms / 1000.0f);
        } else {
            startConnect(host, port, _wait_header_ms / 1000.0f);
        }
    } else {
        SockException ex;
        onConnect_l(ex);
    }
}

void HttpClient::clear() {
    _url.clear();
    _user_set_header.clear();
    _body.reset();
    _method.clear();
    clearResponse();
}

void HttpClient::clearResponse() {
    _complete = false;
    _header_recved = false;
    _recved_body_size = 0;
    _total_body_size = 0;
    _parser.clear();
    _chunked_splitter = nullptr;
    _wait_header.resetTime();
    _wait_body.resetTime();
    _wait_complete.resetTime();
    HttpRequestSplitter::reset();
}

void HttpClient::setMethod(string method) {
    _method = std::move(method);
}

void HttpClient::setHeader(HttpHeader header) {
    _user_set_header = std::move(header);
}

HttpClient &HttpClient::addHeader(string key, string val, bool force) {
    if (!force) {
        _user_set_header.emplace(std::move(key), std::move(val));
    } else {
        _user_set_header[std::move(key)] = std::move(val);
    }
    return *this;
}

void HttpClient::setBody(string body) {
    _body.reset(new HttpStringBody(std::move(body)));
}

void HttpClient::setBody(HttpBody::Ptr body) {
    _body = std::move(body);
}

const Parser &HttpClient::response() const {
    return _parser;
}

ssize_t HttpClient::responseBodyTotalSize() const {
    return _total_body_size;
}

size_t HttpClient::responseBodySize() const {
    return _recved_body_size;
}

const string &HttpClient::getUrl() const {
    return _url;
}

void HttpClient::onConnect(const SockException &ex) {
    onConnect_l(ex);
}

void HttpClient::onConnect_l(const SockException &ex) {
    if (ex) {
        onResponseCompleted_l(ex);
        return;
    }
    _StrPrinter printer;
    // No proxy is used or the proxy server has connected successfully
    if (_proxy_connected || !isUsedProxy()) {
        printer << _method + " " << _path + " HTTP/1.1\r\n";
        for (auto &pr : _header) {
            printer << pr.first + ": ";
            printer << pr.second + "\r\n";
        }
        _header.clear();
        _path.clear();
    } else {
        printer << "CONNECT " << _last_host << " HTTP/1.1\r\n";
        printer << "Proxy-Connection: keep-alive\r\n";
        if (!_proxy_auth.empty()) {
            printer << "Proxy-Authorization: Basic " << _proxy_auth << "\r\n";
        }
    }
    SockSender::send(printer << "\r\n");
    onFlush();
}

void HttpClient::onRecv(const Buffer::Ptr &pBuf) {
    _wait_body.resetTime();
    HttpRequestSplitter::input(pBuf->data(), pBuf->size());
}

void HttpClient::onError(const SockException &ex) {
    if (ex.getErrCode() == Err_reset && _allow_resend_request && _http_persistent && _recved_body_size == 0 && !_header_recved) {
        // The connection was reset, possibly because the server actively closed the connection, or the server kernel parameters or firewall's persistent connection idle timeout or inconsistency.
        // If it is a persistent connection, we can solve this problem by reconnecting
        // The connection was reset, possibly because the server actively disconnected the connection,
        // or the persistent connection idle time of the server kernel parameters or firewall timed out or inconsistent.
        // If it is a persistent connection, then we can solve this problem by reconnecting
        WarnL << "http persistent connect reset, try reconnect";
        _http_persistent = false;
        sendRequest(_url);
        return;
    }
    onResponseCompleted_l(ex);
}

ssize_t HttpClient::onRecvHeader(const char *data, size_t len) {
    _parser.parse(data, len);
    if (_parser.status() == "302" || _parser.status() == "301" || _parser.status() == "303") {
        auto new_url = Parser::mergeUrl(_url, _parser["Location"]);
        if (new_url.empty()) {
            throw invalid_argument("Location field not found (jump url)");
        }
        if (onRedirectUrl(new_url, _parser.status() == "302")) {
            HttpClient::sendRequest(new_url);
            return 0;
        }
    }

    checkCookie(_parser.getHeader());
    onResponseHeader(_parser.status(), _parser.getHeader());
    _header_recved = true;

    if (_parser["Transfer-Encoding"] == "chunked") {
        // If the Transfer-Encoding field is equal to chunked, it is considered that the subsequent content is unlimited in length
        _total_body_size = -1;
        _chunked_splitter = std::make_shared<HttpChunkedSplitter>([this](const char *data, size_t len) {
            if (len > 0) {
                _recved_body_size += len;
                onResponseBody(data, len);
            } else {
                _total_body_size = _recved_body_size;
                if (_recved_body_size > 0) {
                    onResponseCompleted_l(SockException(Err_success, "success"));
                } else {
                    onResponseCompleted_l(SockException(Err_other, "no body"));
                }
            }
        });
        // The following is a continuous body
        return -1;
    }

    if (!_parser["Content-Length"].empty()) {
        // Ignore the return value of onResponseHeader when there is a Content-Length field
        _total_body_size = atoll(_parser["Content-Length"].data());
    } else {
        _total_body_size = -1;
    }

    if (_total_body_size == 0) {
        // There is no content afterwards, this http request ends
        onResponseCompleted_l(SockException(Err_success, "The request is successful but has no body"));
        return 0;
    }

    // When _total_body_size != 0, it means there is content afterwards
    // Although we know the exact size of the content when _total_body_size > 0,
    // But because we don't need to wait for the content to be received before calling onRecvContent (because this wastes memory and requires multiple data copies)
    // So returning -1 means we will receive the content in segments next
    _recved_body_size = 0;
    return -1;
}

void HttpClient::onRecvContent(const char *data, size_t len) {
    if (_chunked_splitter) {
        _chunked_splitter->input(data, len);
        return;
    }
    _recved_body_size += len;
    if (_total_body_size < 0) {
        // Unlimited length content
        onResponseBody(data, len);
        return;
    }

    // Fixed length content
    if (_recved_body_size < (size_t) _total_body_size) {
        // Content has not been received yet
        onResponseBody(data, len);
        return;
    }

    if (_recved_body_size == (size_t)_total_body_size) {
        // Content received
        onResponseBody(data, len);
        onResponseCompleted_l(SockException(Err_success, "completed"));
        return;
    }

    // The declared content data is smaller than the real one, disconnect
    onResponseBody(data, len);
    throw invalid_argument("http response content size bigger than expected");
}

void HttpClient::onFlush() {
    GET_CONFIG(uint32_t, send_buf_size, Http::kSendBufSize);
    while (_body && _body->remainSize() && !isSocketBusy()) {
        auto buffer = _body->readData(send_buf_size);
        if (!buffer) {
            // Data transmission ends or data reading exception
            break;
        }
        if (send(buffer) <= 0) {
            // Data transmission failed, no need to roll back data, because the socket is writable before sending
            // So the send buffer is definitely not full, this buffer must have been written to the socket
            break;
        }
    }
}

void HttpClient::onManager() {
    // The onManager callback is only called when the connection is in progress or connected

    if (_wait_complete_ms > 0) {
        // Total timeout is set
        if (!_complete && _wait_complete.elapsedTime() > _wait_complete_ms) {
            // Timeout waiting for http reply to finish
            shutdown(SockException(Err_timeout, "wait http response complete timeout"));
            return;
        }
        return;
    }

    // Total timeout is not set
    if (!_header_recved) {
        // Waiting for header
        if (_wait_header.elapsedTime() > _wait_header_ms) {
            // Timeout waiting for header
            shutdown(SockException(Err_timeout, "wait http response header timeout"));
            return;
        }
    } else if (_wait_body_ms > 0 && _wait_body.elapsedTime() > _wait_body_ms) {
        // Waiting for body, timeout
        shutdown(SockException(Err_timeout, "wait http response body timeout"));
        return;
    }
}

void HttpClient::onResponseCompleted_l(const SockException &ex) {
    if (_complete) {
        return;
    }
    _complete = true;
    _wait_complete.resetTime();

    if (!ex) {
        // Confirmed success
        onResponseCompleted(ex);
        return;
    }
    // Suspicious failure

    if (_total_body_size > 0 && _recved_body_size >= (size_t)_total_body_size) {
        // If the response header contains content-length information, then the received body is considered successful if it is greater than or equal to the declared value
        onResponseCompleted(SockException(Err_success, "read body completed"));
        return;
    }

    if (_total_body_size == -1 && _recved_body_size > 0) {
        // If the response header does not contain content-length information, then receiving any body is considered successful
        onResponseCompleted(SockException(Err_success, ex.what()));
        return;
    }

    // Confirmed failure
    onResponseCompleted(ex);
}

bool HttpClient::waitResponse() const {
    return !_complete && alive();
}

bool HttpClient::isHttps() const {
    return _is_https;
}

void HttpClient::checkCookie(HttpClient::HttpHeader &headers) {
    //Set-Cookie: IPTV_SERVER=8E03927B-CC8C-4389-BC00-31DBA7EC7B49;expires=Sun, Sep 23 2018 15:07:31 GMT;path=/index/api/
    for (auto it_set_cookie = headers.find("Set-Cookie"); it_set_cookie != headers.end(); ++it_set_cookie) {
        auto key_val = Parser::parseArgs(it_set_cookie->second, ";", "=");
        HttpCookie::Ptr cookie = std::make_shared<HttpCookie>();
        cookie->setHost(_last_host);

        int index = 0;
        auto arg_vec = split(it_set_cookie->second, ";");
        for (string &key_val : arg_vec) {
            auto key = findSubString(key_val.data(), NULL, "=");
            auto val = findSubString(key_val.data(), "=", NULL);

            if (index++ == 0) {
                cookie->setKeyVal(key, val);
                continue;
            }

            if (key == "path") {
                cookie->setPath(val);
                continue;
            }

            if (key == "expires") {
                cookie->setExpires(val, headers["Date"]);
                continue;
            }
        }

        if (!(*cookie)) {
            // Invalid cookie
            continue;
        }
        HttpCookieStorage::Instance().set(cookie);
    }
}

void HttpClient::setHeaderTimeout(size_t timeout_ms) {
    CHECK(timeout_ms > 0);
    _wait_header_ms = timeout_ms;
}

void HttpClient::setBodyTimeout(size_t timeout_ms) {
    _wait_body_ms = timeout_ms;
}

void HttpClient::setCompleteTimeout(size_t timeout_ms) {
    _wait_complete_ms = timeout_ms;
}

bool HttpClient::isUsedProxy() const {
    return _used_proxy;
}

bool HttpClient::isProxyConnected() const {
    return _proxy_connected;
}

void HttpClient::setProxyUrl(string proxy_url) {
    _proxy_url = std::move(proxy_url);
    if (!_proxy_url.empty()) {
        parseProxyUrl(_proxy_url, _proxy_host, _proxy_port, _proxy_auth);
        _used_proxy = true;
    } else {
        _used_proxy = false;
    }
}

bool HttpClient::checkProxyConnected(const char *data, size_t len) {
    auto ret = strstr(data, "HTTP/1.1 200 Connection established");
    _proxy_connected = ret != nullptr;
    return _proxy_connected;
}

void HttpClient::setAllowResendRequest(bool allow) {
    _allow_resend_request = allow;
}
} /* namespace mediakit */
