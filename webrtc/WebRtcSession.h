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

class WebRtcSession : public toolkit::Session, public HttpRequestSplitter {
public:
    WebRtcSession(const toolkit::Socket::Ptr &sock);

    void attachServer(const toolkit::Server &server) override;
    void onRecv(const toolkit::Buffer::Ptr &) override;
    void onError(const toolkit::SockException &err) override;
    void onManager() override;
    static toolkit::EventPoller::Ptr queryPoller(const toolkit::Buffer::Ptr &buffer);

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
    toolkit::Ticker _ticker;
    std::weak_ptr<toolkit::TcpServer> _server;
};

}// namespace mediakit

#endif //S3MEDIAKIT_WEBRTCSESSION_H
