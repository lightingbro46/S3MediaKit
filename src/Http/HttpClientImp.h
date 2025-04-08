#ifndef SRC_HTTP_HTTPCLIENTIMP_H_
#define SRC_HTTP_HTTPCLIENTIMP_H_

#include "HttpClient.h"
#include "Util/SSLBox.h"

namespace mediakit {

class HttpClientImp : public toolkit::TcpClientWithSSL<HttpClient> {
public:
    using Ptr = std::shared_ptr<HttpClientImp>;

protected:
    void onConnect(const toolkit::SockException &ex) override;
    ssize_t onRecvHeader(const char *data, size_t len) override;
};

} /* namespace mediakit */
#endif /* SRC_HTTP_HTTPCLIENTIMP_H_ */
