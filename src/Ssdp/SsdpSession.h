#ifndef S3MEDIAKIT_SSDP_SESSION_H
#define S3MEDIAKIT_SSDP_SESSION_H

#include "Network/Session.h"

namespace mediakit {

class SsdpSession : public toolkit::Session {
public:
    SsdpSession(const toolkit::Socket::Ptr &sock);

    void onRecv(const toolkit::Buffer::Ptr &) override;
    void onError(const toolkit::SockException &err) override;
    void onManager() override;
    void attachServer(const toolkit::Server &server) override;

private:
    void sendResponse();

private:
    toolkit::Ticker _ticker;
    struct sockaddr_storage _peer_addr;
};

} // namespace mediakit

#endif // S3MEDIAKIT_SSDP_SESSION_H