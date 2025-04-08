#if defined(ENABLE_RTPPROXY)
#include "RtpSession.h"
#include "RtpProcess.h"
#include "Network/TcpServer.h"
#include "Rtsp/Rtsp.h"
#include "Rtsp/RtpReceiver.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit{

const string RtpSession::kVhost = "vhost";
const string RtpSession::kApp = "app";
const string RtpSession::kStreamID = "stream_id";
const string RtpSession::kSSRC = "ssrc";
const string RtpSession::kOnlyTrack = "only_track";
const string RtpSession::kUdpRecvBuffer = "udp_recv_socket_buffer";

void RtpSession::attachServer(const Server &server) {
    setParams(const_cast<Server &>(server));
}

void RtpSession::setParams(mINI &ini) {
    _tuple.vhost = ini[kVhost];
    _tuple.app = ini[kApp];
    _tuple.stream = ini[kStreamID];
    _ssrc = ini[kSSRC];
    _only_track = ini[kOnlyTrack];
    int udp_socket_buffer = ini[kUdpRecvBuffer];
    if (_is_udp) {
        // Set udp socket read buffer
        SockUtil::setRecvBuf(getSock()->rawFD(),
            (udp_socket_buffer > 0) ? udp_socket_buffer : (4 * 1024 * 1024));
    }
}

RtpSession::RtpSession(const Socket::Ptr &sock)
    : Session(sock) {
    socklen_t addr_len = sizeof(_addr);
    getpeername(sock->rawFD(), (struct sockaddr *)&_addr, &addr_len);
    _is_udp = sock->sockType() == SockNum::Sock_UDP;
}

RtpSession::~RtpSession() = default;

void RtpSession::onRecv(const Buffer::Ptr &data) {
    if (_is_udp) {
        onRtpPacket(data->data(), data->size());
        return;
    }
    RtpSplitter::input(data->data(), data->size());
}

void RtpSession::onError(const SockException &err) {
    if (_emit_detach) {
        _process->onDetach(err);
    }
    WarnP(this) << _tuple.shortUrl() << " " << err;
}

void RtpSession::onManager() {
    if (!_process && _ticker.createdTime() > 10 * 1000) {
        shutdown(SockException(Err_timeout, "illegal connection"));
    }
}

void RtpSession::setRtpProcess(RtpProcess::Ptr process) {
    _emit_detach = (bool)process;
    _process = std::move(process);
}

void RtpSession::onRtpPacket(const char *data, size_t len) {
    if (!isRtp(data, len)) {
        // Ignore non-rtp data
        WarnP(this) << "Not rtp packet";
        return;
    }
    if (!_is_udp) {
        if (_search_rtp) {
            // Data discarded during context search
            if (_search_rtp_finished) {
                // The next packet is the correct rtp packet
                _search_rtp_finished = false;
                _search_rtp = false;
            }
            return;
        }
        GET_CONFIG(uint32_t, rtpMaxSize, Rtp::kRtpMaxSize);
        if (len > 1024 * rtpMaxSize) {
            _search_rtp = true;
            WarnL << "RTP packet length exception(" << len << "), the sender may cache overflow and overwrite, start searching for ssrc in order to restore the context";
            return;
        }
    }

    // Try to get ssrc when ssrc is not set
    if (!_ssrc && !getSSRC(data, len, _ssrc)) {
        return;
    }

    // Use ssrc as stream id if stream id is not specified
    if (_tuple.stream.empty()) {
        _tuple.stream = printSSRC(_ssrc);
    }

    if (!_process) {
        _process = RtpProcess::createProcess(_tuple);
        _process->setOnlyTrack((RtpProcess::OnlyTrack)_only_track);
        weak_ptr<RtpSession>  weak_self = static_pointer_cast<RtpSession>(shared_from_this());
        _process->setOnDetach([weak_self](const SockException &ex) {
            if (auto strong_self = weak_self.lock()) {
                strong_self->safeShutdown(ex);
            }
        });
    }
    try {
        uint32_t rtp_ssrc = 0;
        getSSRC(data, len, rtp_ssrc);
        if (rtp_ssrc != _ssrc) {
            WarnP(this) << "ssrc mismatched, rtp dropped: " << rtp_ssrc << " != " << _ssrc;
            return;
        }
        _process->inputRtp(false, getSock(), data, len, (struct sockaddr *)&_addr);
    } catch (RtpTrack::BadRtpException &ex) {
        if (!_is_udp) {
            WarnL << ex.what() << ", start searching for ssrc to restore context";
            _search_rtp = true;
        } else {
            throw;
        }
    }
    _ticker.resetTime();
}

static const char *findSSRC(const char *data, ssize_t len, uint32_t ssrc) {
    // Two bytes of length field must be reserved before rtp
    for (ssize_t i = 2; i <= len - 4; ++i) {
        auto ptr = (const uint8_t *)data + i;
        if (ptr[0] == (ssrc >> 24) && ptr[1] == ((ssrc >> 16) & 0xFF) && ptr[2] == ((ssrc >> 8) & 0xFF)
            && ptr[3] == (ssrc & 0xFF)) {
            return (const char *)ptr;
        }
    }
    return nullptr;
}

static const char *findPsHeaderFlag(const char *data, ssize_t len) {
    for (ssize_t i = 2; i <= len - 4; ++i) {
        auto ptr = (const uint8_t *)data + i;
        // PsHeader 0x000001ba, PsSystemHeader 0x000001bb (keyframe identifier)
        if (ptr[0] == (0x00) && ptr[1] == (0x00) && ptr[2] == (0x01) && ptr[3] == (0xbb)) {
            return (const char *)ptr;
        }
    }

    return nullptr;
}

// The length between rtp length and ssrc is fixed to 10
static size_t constexpr kSSRCOffset = 2 + 4 + 4;
// The length between rtp length and ps header is fixed to 14 (temporarily not using ps header, using system header instead)
// The length between rtp length and ps system header is fixed to 20 (keyframe identifier)
static size_t constexpr kPSHeaderOffset = 2 + 4 + 4 + 4 + 20;

const char *RtpSession::onSearchPacketTail(const char *data, size_t len) {
    if (!_search_rtp) {
        // Tcp context is normal, no need to search ssrc
        return RtpSplitter::onSearchPacketTail(data, len);
    }
    if (!_process) {
        InfoL << "Ssrc has not been obtained, and the tcp context cannot be restored through Ssrc; try to search for PsSystemHeader to restore the tcp context.";
        auto rtp_ptr1 = searchByPsHeaderFlag(data, len);
        return rtp_ptr1;
    }
    auto rtp_ptr0 = searchBySSRC(data, len);
    if (rtp_ptr0) {
        return rtp_ptr0;
    }
    // Continue to search for ps header flag if ssrc search fails
    auto rtp_ptr2 = searchByPsHeaderFlag(data, len);
    return rtp_ptr2;
}

const char *RtpSession::searchBySSRC(const char *data, size_t len) {
    InfoL << "Try rtp search ssrc..._ssrc=" << _ssrc;
    // Search for the first rtp's ssrc
    auto ssrc_ptr0 = findSSRC(data, len, _ssrc);
    if (!ssrc_ptr0) {
        // Return insufficient data if no rtp is found
        InfoL << "rtp search ssrc failed (the first data is not enough), and the rtp data is discarded：" << len;
        return nullptr;
    }
    // These two bytes are the length field of the first rtp
    auto rtp_len_ptr = (ssrc_ptr0 - kSSRCOffset);
    auto rtp_len = ((uint8_t *)rtp_len_ptr)[0] << 8 | ((uint8_t *)rtp_len_ptr)[1];

    // Search for the second rtp's ssrc
    auto ssrc_ptr1 = findSSRC(ssrc_ptr0 + rtp_len, data + (ssize_t)len - ssrc_ptr0 - rtp_len, _ssrc);
    if (!ssrc_ptr1) {
        // Return insufficient data if the second rtp is not found
        InfoL << "The rtp search for ssrc fails (the second data is not enough), and the rtp data is discarded as: " << len;
        return nullptr;
    }

    // The interval between the two ssrcs is exactly equal to the length of the rtp (plus the rtp length field), which means that the rtp is found
    auto ssrc_offset = ssrc_ptr1 - ssrc_ptr0;
    if (ssrc_offset == rtp_len + 2 || ssrc_offset == rtp_len + 4) {
        InfoL << "RTP search ssrc successfully, tcp context recovery is successful, and the discarded RTP residual data is：" << rtp_len_ptr - data;
        _search_rtp_finished = true;
        if (rtp_len_ptr == data) {
            // Stop searching for rtp, otherwise it will enter an infinite loop
            _search_rtp = false;
        }
        // All previous data needs to be discarded, this is the start of rtp
        return rtp_len_ptr;
    }
    // The length of the first rtp does not match, which means that the first ssrc found is not rtp, discard it, we start searching from the second ssrc rtp
    return ssrc_ptr1 - kSSRCOffset;
}

const char *RtpSession::searchByPsHeaderFlag(const char *data, size_t len) {
    InfoL << "Try rtp search for PsSystemHeaderFlag..._ssrc=" << _ssrc;
    // Search for the first PsHeaderFlag in rtp
    auto ps_header_flag_ptr = findPsHeaderFlag(data, len);
    if (!ps_header_flag_ptr) {
        InfoL << "The rtp search flag failed, discarded rtp data as：" << len;
        return nullptr;
    }

    auto rtp_ptr = ps_header_flag_ptr - kPSHeaderOffset;
    _search_rtp_finished = true;
    if (rtp_ptr == data) {
        // Stop searching for rtp, otherwise it will enter an infinite loop
        _search_rtp = false;
    }
    InfoL << "The rtp search flag is successful, the tcp context is restored successfully, and the discarded rtp residual data is:" << rtp_ptr - data;

    // TODO or Not ? Update setting ssrc
    uint32_t rtp_ssrc = 0;
    getSSRC(rtp_ptr + 2, len, rtp_ssrc);
    _ssrc = rtp_ssrc;
    InfoL << "Set _ssrc to：" << _ssrc;
    // RtpServer::updateSSRC(uint32_t ssrc)
    return rtp_ptr;
}

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
