#include "Http/HttpClientImp.h"

using namespace toolkit;

namespace mediakit {

void HttpClientImp::onConnect(const SockException &ex) {
    if (isUsedProxy() && !isProxyConnected()) {
        // Connect to the proxy server
        setDoNotUseSSL();
        HttpClient::onConnect(ex);
    } else {
        if (!isHttps()) {
            // When https 302 redirects to http, ssl needs to be closed
            setDoNotUseSSL();
            HttpClient::onConnect(ex);
        } else {
            TcpClientWithSSL<HttpClient>::onConnect(ex);
        }
    }
}

ssize_t HttpClientImp::onRecvHeader(const char *data, size_t len) {
    if (isUsedProxy() && !isProxyConnected()) {
        if (checkProxyConnected(data, len)) {
            clearResponse();
            onConnect(SockException(Err_success, "proxy connected"));
            return 0;
        }
    }
    return HttpClient::onRecvHeader(data, len);
}

} /* namespace mediakit */
