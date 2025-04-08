#ifndef S3MEDIAKIT_WEBRTCSESSION_H
#define S3MEDIAKIT_WEBRTCSESSION_H

#include "WebRtcTransport.h"
#include "Network/Session.h"
#include "Http/HttpRequestSplitter.h"

namespace toolkit {
    class TcpServer;
}

namespace mediakit {
class WebRtcTransportImp;
using namespace toolkit;

class WebRtcSession : public Session, public HttpRequestSplitter {
public:
    WebRtcSession(const Socket::Ptr &sock);

    void attachServer(const Server &server) override;
    void onRecv(const Buffer::Ptr &) override;
    void onError(const SockException &err) override;
    void onManager() override;
    static EventPoller::Ptr queryPoller(const Buffer::Ptr &buffer);

protected:
    WebRtcTransportImp::Ptr _transport;

private:
    //// HttpRequestSplitter override ////
    ssize_t onRecvHeader(const char *data, size_t len) override;
    const char *onSearchPacketTail(const char *data, size_t len) override;

    void onRecv_l(const char *data, size_t len);

private:
    bool _over_tcp = false;
    bool _find_transport = true;
    Ticker _ticker;
    std::weak_ptr<toolkit::TcpServer> _server;
};

}// namespace mediakit

#endif //S3MEDIAKIT_WEBRTCSESSION_H
