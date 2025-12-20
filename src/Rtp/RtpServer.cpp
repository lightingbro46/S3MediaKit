#if defined(ENABLE_RTPPROXY)
#include "Util/uv_errno.h"
#include "RtpServer.h"
#include "RtpProcess.h"
#include "Rtcp/RtcpContext.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit{

RtpServer::~RtpServer() {
    if (_on_cleanup) {
        _on_cleanup();
    }
}

class RtcpHelper: public std::enable_shared_from_this<RtcpHelper> {
public:
    using Ptr = std::shared_ptr<RtcpHelper>;

    RtcpHelper(Socket::Ptr rtcp_sock, MediaTuple tuple) {
        _rtcp_sock = std::move(rtcp_sock);
        _tuple = std::move(tuple);
    }

    void setRtpServerInfo(uint16_t local_port, RtpServer::TcpMode mode, bool re_use_port, uint32_t ssrc, int only_track) {
        _ssrc = ssrc;
        _process = RtpProcess::createProcess(_tuple);
        _process->setOnlyTrack((RtpProcess::OnlyTrack)only_track);

        _timeout_cb = [=]() mutable {
            NOTICE_EMIT(BroadcastRtpServerTimeoutArgs, Broadcast::kBroadcastRtpServerTimeout, local_port, _tuple, (int)mode, re_use_port, ssrc);
        };

        weak_ptr<RtcpHelper> weak_self = shared_from_this();
        _process->setOnDetach([weak_self](const SockException &ex) {
            if (auto strong_self = weak_self.lock()) {
                if (strong_self->_on_detach) {
                    strong_self->_on_detach(ex);
                }
                if (ex.getErrCode() == Err_timeout) {
                    strong_self->_timeout_cb();
                }
            }
        });
    }

    void setOnDetach(RtpProcess::onDetachCB cb) { _on_detach = std::move(cb); }

    RtpProcess::Ptr getProcess() const { return _process; }

    void onRecvRtp(const Socket::Ptr &sock, const Buffer::Ptr &buf, struct sockaddr *addr) {
        try {
            _process->inputRtp(true, sock, buf->data(), buf->size(), addr);
        } catch (std::exception &ex) {
            _process->onDetach(SockException(Err_shutdown, ex.what()));
            return;
        }
        // Count RTP reception status, used to send RR packets
        auto header = (RtpHeader *)buf->data();
        sendRtcp(ntohl(header->ssrc), addr);
    }

    void startRtcp() {
        weak_ptr<RtcpHelper> weak_self = shared_from_this();
        _rtcp_sock->setOnRead([weak_self](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) {
            // Used to receive RTCP hole punching packets
            auto strong_self = weak_self.lock();
            if (!strong_self || !strong_self->_process) {
                return;
            }
            if (!strong_self->_rtcp_addr) {
                // Set the RTCP peer port only once
                strong_self->_rtcp_addr = std::make_shared<struct sockaddr_storage>();
                memcpy(strong_self->_rtcp_addr.get(), addr, addr_len);
            }
            auto rtcps = RtcpHeader::loadFromBytes(buf->data(), buf->size());
            for (auto &rtcp : rtcps) {
                strong_self->_process->onRtcp(rtcp);
            }
            // After receiving SR RTCP, the driver returns RR RTCP
            strong_self->sendRtcp(strong_self->_ssrc, (struct sockaddr *)(strong_self->_rtcp_addr.get()));
        });
    }

private:
    void sendRtcp(uint32_t rtp_ssrc, struct sockaddr *addr) {
        // Send RTCP every 5 seconds
        if (_ticker.elapsedTime() < 5000) {
            return;
        }
        _ticker.resetTime();

        auto rtcp_addr = (struct sockaddr *)_rtcp_addr.get();
        if (!rtcp_addr) {
            // By default, the RTCP port is the RTP port + 1
            switch (addr->sa_family) {
                case AF_INET: ((sockaddr_in *)addr)->sin_port = htons(ntohs(((sockaddr_in *)addr)->sin_port) + 1); break;
                case AF_INET6: ((sockaddr_in6 *)addr)->sin6_port = htons(ntohs(((sockaddr_in6 *)addr)->sin6_port) + 1); break;
            }
            // When no RTCP hole punching packet is received, the default RTCP port is used
            rtcp_addr = addr;
        }
        _rtcp_sock->send(_process->createRtcpRR(rtp_ssrc + 1, rtp_ssrc), rtcp_addr);
    }

private:
    uint32_t _ssrc = 0;
    std::function<void()> _timeout_cb;
    Ticker _ticker;
    Socket::Ptr _rtcp_sock;
    RtpProcess::Ptr _process;
    MediaTuple _tuple;
    RtpProcess::onDetachCB _on_detach;
    std::shared_ptr<struct sockaddr_storage> _rtcp_addr;
};

void RtpServer::start(uint16_t local_port, const char *local_ip, const MediaTuple &tuple, TcpMode tcp_mode, bool re_use_port, uint32_t ssrc, int only_track, bool multiplex) {
    // Create UDP server
    auto poller = EventPollerPool::Instance().getPoller();
    Socket::Ptr rtp_socket = Socket::createSocket(poller, true);
    Socket::Ptr rtcp_socket = Socket::createSocket(poller, true);
    if (local_port == 0) {
        // Random port, RTP port uses even numbers
        auto pair = std::make_pair(rtp_socket, rtcp_socket);
        makeSockPair(pair, local_ip, re_use_port);
        local_port = rtp_socket->get_local_port();
    } else if (!rtp_socket->bindUdpSock(local_port, local_ip, re_use_port)) {
        // User-specified port
        throw std::runtime_error(StrPrinter << "Create RTP port " << local_ip << ":" << local_port << " failed:" << get_uv_errmsg(true));
    } else if (!rtcp_socket->bindUdpSock(local_port + 1, local_ip, re_use_port)) {
        // RTCP port
        throw std::runtime_error(StrPrinter << "Create rtcp port " << local_ip << ":" << local_port + 1 << " failed:" << get_uv_errmsg(true));
    }

    // Set UDP socket read cache
    GET_CONFIG(int, udpRecvSocketBuffer, RtpProxy::kUdpRecvSocketBuffer);
    SockUtil::setRecvBuf(rtp_socket->rawFD(), udpRecvSocketBuffer);

    // Create UDP server
    UdpServer::Ptr udp_server;
    RtcpHelper::Ptr helper;
    // Added multiplexing judgment. If multiplexing is true, then go to the else logic, while retaining the original stream_id is empty to go to the else logic
    if (!tuple.stream.empty() && !multiplex) {
        // If a stream ID is specified, then one port is one stream (regardless of whether it contains multiple streams with multiple SSRCs, after binding the RTP source, streams that do not match the IP port will be filtered out)
        helper = std::make_shared<RtcpHelper>(std::move(rtcp_socket), tuple);
        helper->startRtcp();
        helper->setRtpServerInfo(local_port, tcp_mode, re_use_port, ssrc, only_track);
        bool bind_peer_addr = false;
        auto ssrc_ptr = std::make_shared<uint32_t>(ssrc);
        _ssrc = ssrc_ptr;
        rtp_socket->setOnRead([rtp_socket, helper, ssrc_ptr, bind_peer_addr](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) mutable {
            RtpHeader *header = (RtpHeader *)buf->data();
            auto rtp_ssrc = ntohl(header->ssrc);
            auto ssrc = *ssrc_ptr;
            if (ssrc && rtp_ssrc != ssrc) {
                WarnL << "ssrc mismatched, rtp dropped: " << rtp_ssrc << " != " << ssrc;
            } else {
                if (!bind_peer_addr) {
                    // Bind the peer IP + port to prevent multiple devices or one device from pushing multiple streams, resulting in log reports of mismatched SSRCs
                    bind_peer_addr = true;
                    rtp_socket->bindPeerAddr(addr, addr_len);
                }
                helper->onRecvRtp(rtp_socket, buf, addr);
            }
        });
    } else {
        // Single-port multi-threaded reception of multiple streams, distinguishing streams based on SSRC
        udp_server = std::make_shared<UdpServer>();
        (*udp_server)[RtpSession::kOnlyTrack] = only_track;
        (*udp_server)[RtpSession::kUdpRecvBuffer] = udpRecvSocketBuffer;
        (*udp_server)[RtpSession::kVhost] = tuple.vhost;
        (*udp_server)[RtpSession::kApp] = tuple.app;
        udp_server->start<RtpSession>(local_port, local_ip);
        rtp_socket = nullptr;
    }

    TcpServer::Ptr tcp_server;
    if (tcp_mode == PASSIVE || tcp_mode == ACTIVE) {
        auto processor = helper ? helper->getProcess() : nullptr;
        // If the same processor object is shared, declare the TCP server in single-threaded mode to ensure thread safety.
        tcp_server = std::make_shared<TcpServer>(processor ? poller : nullptr);
        (*tcp_server)[RtpSession::kVhost] = tuple.vhost;
        (*tcp_server)[RtpSession::kApp] = tuple.app;
        (*tcp_server)[RtpSession::kStreamID] = tuple.stream;
        (*tcp_server)[RtpSession::kSSRC] = ssrc;
        (*tcp_server)[RtpSession::kOnlyTrack] = only_track;
        if (tcp_mode == PASSIVE) {
            weak_ptr<RtpServer> weak_self = shared_from_this();
            tcp_server->start<RtpSession>(local_port, local_ip, 1024, [weak_self, processor](std::shared_ptr<RtpSession> &session) {
                session->setRtpProcess(processor);
            });
        } else if (tuple.stream.empty()) {
            // In TCP active mode, only one port can have one stream, and the stream ID must be specified; the TcpServer object is created only for parameter passing
            throw std::runtime_error(StrPrinter << "In tcp active mode, the stream id must be specified.");
        }
    }

    _on_cleanup = [rtp_socket]() {
        if (rtp_socket) {
            // Remove circular references
            rtp_socket->setOnRead(nullptr);
        }
    };

    _tcp_server = tcp_server;
    _udp_server = udp_server;
    _rtp_socket = rtp_socket;
    _rtcp_helper = helper;
    _tcp_mode = tcp_mode;
}

void RtpServer::setOnDetach(RtpProcess::onDetachCB cb) {
    if (_rtcp_helper) {
        _rtcp_helper->setOnDetach(std::move(cb));
    }
}

uint16_t RtpServer::getPort() {
    return _udp_server ? _udp_server->getPort() : _rtp_socket->get_local_port();
}

void RtpServer::connectToServer(const std::string &url, uint16_t port, const function<void(const SockException &ex)> &cb) {
    if (_tcp_mode != ACTIVE || !_rtp_socket) {
        cb(SockException(Err_other, "Only support tcp active mode"));
        return;
    }
    weak_ptr<RtpServer> weak_self = shared_from_this();
    _rtp_socket->connect(url, port, [url, port, cb, weak_self](const SockException &err) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            cb(SockException(Err_other, "The service object has been released"));
            return;
        }
        if (err) {
            WarnL << "Connect to the server " << url << ":" << port << " failed " << err;
        } else {
            InfoL << "Connect to the server " << url << ":" << port << " success";
            strong_self->onConnect();
        }
        cb(err);
    },
    5.0F, "::", _rtp_socket->get_local_port());
}

void RtpServer::onConnect() {
    auto rtp_session = std::make_shared<RtpSession>(_rtp_socket);
    rtp_session->setRtpProcess(_rtcp_helper->getProcess());
    rtp_session->attachServer(*_tcp_server);
    _rtp_socket->setOnRead([rtp_session](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) {
        rtp_session->onRecv(buf);
    });
    weak_ptr<RtpServer> weak_self = shared_from_this();
    _rtp_socket->setOnErr([weak_self](const SockException &err) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->_rtp_socket->setOnRead(nullptr);
        }
    });
}

void RtpServer::updateSSRC(uint32_t ssrc) {
    if (_ssrc) {
        *_ssrc = ssrc;
    }

    if (_tcp_server) {
        (*_tcp_server)[RtpSession::kSSRC] = ssrc;
    }
}

uint32_t RtpServer::getSSRC() const {
    if (_ssrc) {
        return *_ssrc;
    }
    if (_tcp_server) {
        return (*_tcp_server)[RtpSession::kSSRC];
    }
    return 0;
}

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
