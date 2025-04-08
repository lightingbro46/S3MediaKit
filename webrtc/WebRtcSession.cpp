#include "WebRtcSession.h"
#include "Util/util.h"
#include "Network/TcpServer.h"
#include "Common/config.h"
#include "IceServer.hpp"
#include "WebRtcTransport.h"

using namespace std;

namespace mediakit {

static string getUserName(const char *buf, size_t len) {
    if (!RTC::StunPacket::IsStun((const uint8_t *) buf, len)) {
        return "";
    }
    std::unique_ptr<RTC::StunPacket> packet(RTC::StunPacket::Parse((const uint8_t *) buf, len));
    if (!packet) {
        return "";
    }
    if (packet->GetClass() != RTC::StunPacket::Class::REQUEST ||
        packet->GetMethod() != RTC::StunPacket::Method::BINDING) {
        return "";
    }
    // Received binding request
    auto vec = split(packet->GetUsername(), ":");
    return vec[0];
}

EventPoller::Ptr WebRtcSession::queryPoller(const Buffer::Ptr &buffer) {
    auto user_name = getUserName(buffer->data(), buffer->size());
    if (user_name.empty()) {
        return nullptr;
    }
    auto ret = WebRtcTransportManager::Instance().getItem(user_name);
    return ret ? ret->getPoller() : nullptr;
}

////////////////////////////////////////////////////////////////////////////////

WebRtcSession::WebRtcSession(const Socket::Ptr &sock) : Session(sock) {
    _over_tcp = sock->sockType() == SockNum::Sock_TCP;
}

void WebRtcSession::attachServer(const Server &server) {
    _server = std::static_pointer_cast<toolkit::TcpServer>(const_cast<Server &>(server).shared_from_this());
}

void WebRtcSession::onRecv_l(const char *data, size_t len) {
    if (_find_transport) {
        // Only allow searching for transport once
        _find_transport = false;
        auto user_name = getUserName(data, len);
        auto transport = WebRtcTransportManager::Instance().getItem(user_name);
        CHECK(transport);

        // WebRtcTransport is on another poller thread, need to switch poller thread and recreate WebRtcSession object
        if (!transport->getPoller()->isCurrentThread()) {
            auto sock = Socket::createSocket(transport->getPoller(), false);
            // 1. Clone socket (fd remains unchanged), switch poller thread to the thread where WebRtcTransport is located
            sock->cloneSocket(*(getSock()));
            auto server = _server;
            std::string str(data, len);
            sock->getPoller()->async([sock, server, str](){
                auto strong_server = server.lock();
                if (strong_server) {
                    auto session = static_pointer_cast<WebRtcSession>(strong_server->createSession(sock));
                    // 2. Create a new WebRtcSession object (bound to the thread where WebRtcTransport is located), reprocess the ice binding request command
                    session->onRecv_l(str.data(), str.size());
                }
            });
            // 3. Destroy the original socket and WebRtcSession (the original object is not on the same thread as WebRtcTransport)
            throw std::runtime_error("webrtc over tcp change poller: " + getPoller()->getThreadName() + " -> " + sock->getPoller()->getThreadName());
        }
        _transport = std::move(transport);
        InfoP(this);
    }
    _ticker.resetTime();
    CHECK(_transport);
    _transport->inputSockData((char *)data, len, this);
}

void WebRtcSession::onRecv(const Buffer::Ptr &buffer) {
    if (_over_tcp) {
        input(buffer->data(), buffer->size());
    } else {
        onRecv_l(buffer->data(), buffer->size());
    }
}

void WebRtcSession::onError(const SockException &err) {
    // UDP connection timeout, but RTC connection may not timeout, because there may be connection migration
    // When UDP connection migrates, the new WebRtcSession object will take over the life cycle of the WebRtcTransport object
    // This WebRtcSession object will be automatically destroyed after timeout
    WarnP(this) << err;

    if (!_transport) {
        return;
    }
    auto self = static_pointer_cast<WebRtcSession>(shared_from_this());
    auto transport = std::move(_transport);
    getPoller()->async([transport, self]() mutable {
        // Delay decrementing the reference count to prevent the object from being destroyed when using the transport object
        transport->removeTuple(self.get());
        // Ensure that the transport is destroyed before the Session object to prevent WebRtcTransport::onDestory() from not being able to get the Session object
        transport = nullptr;
    }, false);
}

void WebRtcSession::onManager() {
    GET_CONFIG(float, timeoutSec, Rtc::kTimeOutSec);
    if (!_transport && _ticker.createdTime() > timeoutSec * 1000) {
        shutdown(SockException(Err_timeout, "illegal webrtc connection"));
        return;
    }
    if (_ticker.elapsedTime() > timeoutSec * 1000) {
        shutdown(SockException(Err_timeout, "webrtc connection timeout"));
        return;
    }
}

ssize_t WebRtcSession::onRecvHeader(const char *data, size_t len) {
    onRecv_l(data + 2, len - 2);
    return 0;
}

const char *WebRtcSession::onSearchPacketTail(const char *data, size_t len) {
    if (len < 2) {
        // Not enough data
        return nullptr;
    }
    uint16_t length = (((uint8_t *)data)[0] << 8) | ((uint8_t *)data)[1];
    if (len < (size_t)(length + 2)) {
        // Not enough data
        return nullptr;
    }
    // Return the end of the RTP packet
    return data + 2 + length;
}

}// namespace mediakit


