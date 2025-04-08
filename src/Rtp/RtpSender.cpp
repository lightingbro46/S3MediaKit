#if defined(ENABLE_RTPPROXY)
#include "RtpSender.h"
#include "RtpSession.h"
#include "Rtsp/RtspSession.h"
#include "Thread/WorkThreadPool.h"
#include "Util/uv_errno.h"
#include "RtpCache.h"
#include "Rtcp/RtcpContext.h"

using namespace std;
using namespace toolkit;

namespace mediakit{

RtpSender::RtpSender(EventPoller::Ptr poller) {
    _poller = poller ? std::move(poller) : EventPollerPool::Instance().getPoller();
    _socket_rtp = Socket::createSocket(_poller, false);
}

RtpSender::~RtpSender() {
    try {
        flush();
    } catch (std::exception &ex) {
        WarnL << "Exception occurred: " << ex.what();
    }
}

void RtpSender::startSend(const MediaSourceEvent::SendRtpArgs &args, const function<void(uint16_t local_port, const SockException &ex)> &cb){
    _args = args;
    if (!_interface) {
        // Do not recreate the object when reconnecting
        auto lam = [this](std::shared_ptr<List<Buffer::Ptr>> list) { onFlushRtpList(std::move(list)); };
        switch (args.data_type) {
            case MediaSourceEvent::SendRtpArgs::kRtpPS: _interface = std::make_shared<RtpCachePS>(lam, atoi(args.ssrc.data()), args.pt, true); break;
            case MediaSourceEvent::SendRtpArgs::kRtpTS: _interface = std::make_shared<RtpCachePS>(lam, atoi(args.ssrc.data()), args.pt, false); break;
            case MediaSourceEvent::SendRtpArgs::kRtpES: _interface = std::make_shared<RtpCacheRaw>(lam, atoi(args.ssrc.data()), args.pt, args.only_audio); break;
            default: CHECK(0, "invalid rtp type: " + to_string(args.data_type)); break;
        }
    }

    auto delay_ms = _args.close_delay_ms ? _args.close_delay_ms : 5000;
    weak_ptr<RtpSender> weak_self = shared_from_this();
    if (args.con_type == MediaSourceEvent::SendRtpArgs::kTcpPassive) {
        auto tcp_listener = Socket::createSocket(_poller, false);
        if (args.src_port) {
            // Specify the port
            if (!tcp_listener->listen(args.src_port)) {
                throw std::invalid_argument(StrPrinter << "open tcp passive server failed on port: " << args.src_port << ", err: " << get_uv_errmsg(true));
            }
        } else {
            auto pr = std::make_pair(tcp_listener, Socket::createSocket(_poller, false));
            // Get a random port from the port pool
            makeSockPair(pr, "::", true, false);
        }
        // The timer holds the tcp_listener to ensure listening within the timeout period
        auto delay_task = _poller->doDelayTask(delay_ms, [weak_self, tcp_listener]() mutable {
            // Prevent circular references
            tcp_listener = nullptr;
            if (auto strong_self = weak_self.lock()) {
                strong_self->onClose(SockException(Err_timeout, "wait tcp connection timeout"));
            }
            return 0;
        });
        tcp_listener->setOnAccept([weak_self, delay_task](Socket::Ptr &sock, std::shared_ptr<void> &complete) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            delay_task->cancel();
            strong_self->_socket_rtp = sock;
            strong_self->onConnect();
            InfoL << "accept tcp connection from: " << sock->get_peer_ip() << ":" << sock->get_peer_port();
        });
        InfoL << "start tcp passive server on: " << tcp_listener->get_local_port();
        cb(tcp_listener->get_local_port(), SockException());

    } else if (args.con_type == MediaSourceEvent::SendRtpArgs::kUdpPassive) {
        if (args.src_port) {
            // Specify the port
            if (!_socket_rtp->bindUdpSock(args.src_port, "::", true)) {
                throw std::invalid_argument(StrPrinter << "open udp passive server failed on port: " << args.src_port << ", err: " << get_uv_errmsg(true));
            }
        } else {
            auto pr = std::make_pair(_socket_rtp, Socket::createSocket(_poller, false));
            // Get a random port from the port pool
            makeSockPair(pr, "::", true, true);
        }
        auto delay_task = _poller->doDelayTask(delay_ms, [weak_self]() mutable {
            if (auto strong_self = weak_self.lock()) {
                // Close the port
                strong_self->_socket_rtp->closeSock();
                strong_self->onClose(SockException(Err_timeout, "wait udp connection timeout"));
            }
            return 0;
        });
        _socket_rtp->setOnRead([weak_self, delay_task](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            delay_task->cancel();
            strong_self->_socket_rtp->bindPeerAddr(addr, addr_len, true);
            // Execute onConnect asynchronously to prevent calling setOnRead in the OnRead callback
            strong_self->_poller->async([strong_self]() { strong_self->onConnect(); }, false);
            InfoL << "accept udp connection from: " << strong_self->_socket_rtp->get_peer_ip() << ":" << strong_self->_socket_rtp->get_peer_port();
        });
        InfoL << "start udp passive server on: " << _socket_rtp->get_local_port();
        cb(_socket_rtp->get_local_port(), SockException());

    } else if (args.con_type == MediaSourceEvent::SendRtpArgs::kUdpActive) {
        auto poller = _poller;
        WorkThreadPool::Instance().getPoller()->async([cb, args, weak_self, poller]() {
            struct sockaddr_storage addr;
            // The purpose of switching threads is to perform DNS resolution in the background thread
            if (!SockUtil::getDomainIP(args.dst_url.data(), args.dst_port, addr, AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
                poller->async([args, cb]() {
                    // Switch back to your own thread
                    cb(0, SockException(Err_dns, StrPrinter << "dns resolution failed: " << args.dst_url));
                });
                return;
            }

            // DNS resolution successful
            poller->async([args, addr, weak_self, cb]() {
                // Switch back to your own thread
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                string ifr_ip = addr.ss_family == AF_INET ? "0.0.0.0" : "::";
                try {
                    if (args.src_port) {
                        // Specify the port
                        if (!strong_self->_socket_rtp->bindUdpSock(args.src_port, ifr_ip, true)) {
                            throw std::invalid_argument(StrPrinter << "open udp active client failed on port: " << args.src_port << ", err: " << get_uv_errmsg(true));
                        }
                    } else {
                        auto pr = std::make_pair(strong_self->_socket_rtp, Socket::createSocket(strong_self->_poller, false));
                        // Get a random port from the port pool
                        makeSockPair(pr, ifr_ip, true, true);
                    }
                } catch (std::exception &ex) {
                    cb(0, SockException(Err_other, ex.what()));
                    return;
                }
                strong_self->_socket_rtp->bindPeerAddr((struct sockaddr *)&addr, 0, true);
                strong_self->onConnect();
                cb(strong_self->_socket_rtp->get_local_port(), SockException());
            });
        });
        InfoL << "start udp active send rtp to: " << args.dst_url << ":" << args.dst_port;

    } else if (args.con_type == MediaSourceEvent::SendRtpArgs::kTcpActive) {
        _socket_rtp->connect(args.dst_url, args.dst_port,[cb, weak_self](const SockException &err) {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                if (!err) {
                    // TCP connection successful
                    strong_self->onConnect();
                }
                cb(strong_self->_socket_rtp->get_local_port(), err);
            } else {
                cb(0, err);
            }
        }, delay_ms / 1000.0, "::", args.src_port);
        InfoL << "start tcp active send rtp to: " << args.dst_url << ":" << args.dst_port;
    } else if (args.con_type == MediaSourceEvent::SendRtpArgs::kVoiceTalk) {
        auto src = MediaSource::find(args.recv_stream_vhost, args.recv_stream_app, args.recv_stream_id);
        if (!src) {
            cb(0, SockException(Err_other, "can not find the target stream"));
            return;
        }
        auto processor = src->getRtpProcess();
        if (!processor) {
            cb(0, SockException(Err_other, "get rtp processor from target stream failed"));
            return;
        }
        auto sock = processor->getSock();
        if (!sock) {
            cb(0, SockException(Err_other, "get sock from rtp processor failed"));
            return;
        }
        _socket_rtp = std::move(sock);
        onConnect();
        cb(_socket_rtp->get_local_port(), SockException());
    } else {
        CHECK(0, "invalid con type");
    }
}

void RtpSender::createRtcpSocket() {
    if (_socket_rtcp) {
        return;
    }
    _socket_rtcp = Socket::createSocket(_socket_rtp->getPoller(), false);
    // The RTCP port is the RTP port + 1
    if(!_socket_rtcp->bindUdpSock(_socket_rtp->get_local_port() + 1, _socket_rtp->get_local_ip(), true)){
        WarnL << "bind rtcp udp socket failed: " << get_uv_errmsg(true);
        _socket_rtcp = nullptr;
        return;
    }

    // Bind the target RTCP port (target RTP port + 1)
    auto addr = SockUtil::make_sockaddr(_socket_rtp->get_peer_ip().data(), _socket_rtp->get_peer_port() + 1);
    _socket_rtcp->bindPeerAddr((struct sockaddr *)&addr, 0, true);

    _rtcp_context = std::make_shared<RtcpContextForSend>();
    weak_ptr<RtpSender> weak_self = shared_from_this();
    bool bind_addr = false;
    _socket_rtcp->setOnRead([weak_self, bind_addr](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) mutable {
        // Receive receive report RTCP
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        if (!bind_addr) {
            // Reply RTCP after receiving the other party's RTCP hole punching packet
            bind_addr = true;
            strong_self->_socket_rtcp->bindPeerAddr(addr, addr_len, true);
        }
        auto rtcp_arr = RtcpHeader::loadFromBytes(buf->data(), buf->size());
        for (auto &rtcp : rtcp_arr) {
            strong_self->onRecvRtcp(rtcp);
        }
    });
    InfoL << "open rtcp port success, start check rr rtcp timeout";
}

void RtpSender::onRecvRtcp(RtcpHeader *rtcp) {
    _rtcp_context->onRtcp(rtcp);
    _rtcp_recv_ticker.resetTime();
}

// Connection established successfully event
void RtpSender::onConnect() {
    _is_connect = true;
    // Increase the send buffer to prevent problems such as UDP packet loss
    SockUtil::setSendBuf(_socket_rtp->rawFD(), 4 * 1024 * 1024);

    if (_args.con_type != MediaSourceEvent::SendRtpArgs::kVoiceTalk) {
        if (_args.con_type == MediaSourceEvent::SendRtpArgs::kTcpActive || _args.con_type == MediaSourceEvent::SendRtpArgs::kTcpPassive) {
            // Close TCP no_delay and enable MSG_MORE to improve sending performance
            SockUtil::setNoDelay(_socket_rtp->rawFD(), false);
            _socket_rtp->setSendFlags(SOCKET_DEFAULE_FLAGS | FLAG_MORE);
        } else if (_args.udp_rtcp_timeout) {
            createRtcpSocket();
        }
        // Connection established successfully event
        weak_ptr<RtpSender> weak_self = shared_from_this();
        if (!_args.recv_stream_id.empty()) {
            mINI ini;
            ini[RtpSession::kStreamID] = _args.recv_stream_id;
            // Force synchronization of the app and vhost of the receive stream and send stream
            ini[RtpSession::kApp] = _args.recv_stream_app;
            ini[RtpSession::kVhost] = _args.recv_stream_vhost;
            _rtp_session = std::make_shared<RtpSession>(_socket_rtp);
            _rtp_session->setParams(ini);

            _socket_rtp->setOnRead([weak_self](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                try {
                    strong_self->_rtp_session->onRecv(buf);
                } catch (std::exception &ex) {
                    SockException err(toolkit::Err_shutdown, ex.what());
                    strong_self->_rtp_session->shutdown(err);
                }
            });
        } else {
            _socket_rtp->setOnRead(nullptr);
        }
        _socket_rtp->setOnErr([weak_self](const SockException &err) {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                strong_self->onErr(err);
            }
        });
    }
    InfoL << "startSend rtp success: " << _socket_rtp->get_peer_ip() << ":" << _socket_rtp->get_peer_port() << ", data_type: " << _args.data_type << ", con_type: " << _args.con_type;
}

bool RtpSender::addTrack(const Track::Ptr &track) {
    if (_args.only_audio && track->getTrackType() == TrackVideo) {
        // Ignore video if only audio is sent
        return false;
    }
    return _interface->addTrack(track);
}

void RtpSender::addTrackCompleted() {
    _interface->addTrackCompleted();
}

void RtpSender::resetTracks() {
    _interface->resetTracks();
}

void RtpSender::flush() {
    if (_interface) {
        _interface->flush();
    }
}

bool RtpSender::inputFrame(const Frame::Ptr &frame) {
    if (_args.only_audio && frame->getTrackType() == TrackVideo) {
        // Ignore video if only audio is sent
        return false;
    }
    // Perform the actual operation after the connection is successful (save CPU resources)
    return _is_connect ? _interface->inputFrame(frame) : false;
}

void RtpSender::onSendRtpUdp(const toolkit::Buffer::Ptr &buf, bool check) {
    if (!_socket_rtcp) {
        return;
    }
    auto rtp = static_pointer_cast<RtpPacket>(buf);
    _rtcp_context->onRtp(rtp->getSeq(), rtp->getStamp(), rtp->ntp_stamp, 90000 /*not used*/, rtp->size());

    if (!check) {
        // Reduce the number of judgments
        return;
    }
    // Send RTCP every 5 seconds
    if (_rtcp_send_ticker.elapsedTime() > _args.rtcp_send_interval_ms) {
        _rtcp_send_ticker.resetTime();
        // rtcp ssrc is rtp ssrc + 1
        auto sr = _rtcp_context->createRtcpSR(atoi(_args.ssrc.data()) + 1);
        // send sender report rtcp
        _socket_rtcp->send(sr);
    }

    if (_rtcp_recv_ticker.elapsedTime() > _args.rtcp_timeout_ms) {
        // Receive rr rtcp timeout
        WarnL << "recv rr rtcp timeout";
        _rtcp_recv_ticker.resetTime();
        onClose(SockException(Err_timeout, "recv rr rtcp timeout"));
    }
}

void RtpSender::onClose(const SockException &ex) {
    auto cb = _on_close;
    if (cb) {
        // Trigger onClose in the next loop to prevent deleting elements while iterating through the map
        _poller->async([cb, ex]() { cb(ex); }, false);
    }
}

// This function is executed in another thread
void RtpSender::onFlushRtpList(shared_ptr<List<Buffer::Ptr>> rtp_list) {
    if (!_is_connect) {
        // Data can only be sent after the connection is successful
        return;
    }

    auto send_func = [this](const shared_ptr<List<Buffer::Ptr>> &rtp_list) {
        size_t i = 0;
        auto size = rtp_list->size();
        rtp_list->for_each([&](Buffer::Ptr &packet) {
            switch (_args.con_type) {
                case MediaSourceEvent::SendRtpArgs::kUdpActive:
                case MediaSourceEvent::SendRtpArgs::kUdpPassive: {
                    onSendRtpUdp(packet, i == 0);
                    // UDP mode, the first 4 bytes of rtp over tcp can be ignored
                    _socket_rtp->send(std::make_shared<BufferRtp>(std::move(packet), RtpPacket::kRtpTcpHeaderSize), nullptr, 0, ++i == size);
                    break;
                }
                case MediaSourceEvent::SendRtpArgs::kTcpActive:
                case MediaSourceEvent::SendRtpArgs::kTcpPassive: {
                    // TCP mode, the first 2 bytes of rtp over tcp can be ignored, only the subsequent 2 bytes of rtp length are retained
                    _socket_rtp->send(std::make_shared<BufferRtp>(std::move(packet), 2), nullptr, 0, ++i == size);
                    break;
                }
                case MediaSourceEvent::SendRtpArgs::kVoiceTalk: {
                    auto type = _socket_rtp->alive() ? _socket_rtp->sockType() : SockNum::Sock_Invalid;
                    if (type == SockNum::Sock_UDP) {
                        _socket_rtp->send(std::make_shared<BufferRtp>(std::move(packet), RtpPacket::kRtpTcpHeaderSize), nullptr, 0, ++i == size);
                    } else if (type == SockNum::Sock_TCP) {
                        _socket_rtp->send(std::make_shared<BufferRtp>(std::move(packet), 2), nullptr, 0, ++i == size);
                    } else {
                        onErr(SockException(Err_other, "dst socket disconnected"));
                    }
                    break;
                }
                default: CHECK(0);
            }
        });
    };
    if (_args.con_type != MediaSourceEvent::SendRtpArgs::kVoiceTalk) {
        weak_ptr<RtpSender> weak_self = shared_from_this();
        _socket_rtp->getPoller()->async([weak_self, rtp_list, send_func]() {
            if (auto strong_self = weak_self.lock()) {
                send_func(rtp_list);
            }
        });
    } else {
        send_func(rtp_list);
    }
}

void RtpSender::onErr(const SockException &ex) {
    _is_connect = false;
    WarnL << "send rtp connection lost: " << ex;
    onClose(ex);
}

void RtpSender::setOnClose(std::function<void(const toolkit::SockException &ex)> on_close) {
    _on_close = std::move(on_close);
}

} // namespace mediakit
#endif // defined(ENABLE_RTPPROXY)
