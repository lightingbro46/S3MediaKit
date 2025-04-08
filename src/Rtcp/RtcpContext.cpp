#include "RtcpContext.h"
#include "Util/logger.h"
using namespace toolkit;

namespace mediakit {

void RtcpContext::onRtp(
    uint16_t /*seq*/, uint32_t stamp, uint64_t ntp_stamp_ms, uint32_t /*sample_rate*/, size_t bytes) {
    ++_packets;
    _bytes += bytes;
    _last_rtp_stamp = stamp;
    _last_ntp_stamp_ms = ntp_stamp_ms;
}

size_t RtcpContext::getExpectedPackets() const {
    throw std::runtime_error("Without implementation, the rtp sender cannot count the number of packets receivable");
}

size_t RtcpContext::getExpectedPacketsInterval() {
    throw std::runtime_error("Without implementation, the rtp sender cannot count the number of packets receivable");
}

size_t RtcpContext::getLost() {
    throw std::runtime_error("Without implementation, rtp sender cannot count the packet loss rate");
}

size_t RtcpContext::getLostInterval() {
    throw std::runtime_error("Without implementation, rtp sender cannot count the packet loss rate");
}

Buffer::Ptr RtcpContext::createRtcpSR(uint32_t rtcp_ssrc) {
    throw std::runtime_error("No implementation, rtp receiver attempts to send sr packet");
}

Buffer::Ptr RtcpContext::createRtcpRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc) {
    throw std::runtime_error("No implementation, rtp sender tries to send rr packet");
}

Buffer::Ptr RtcpContext::createRtcpXRDLRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc) {
    throw std::runtime_error("No implementation, rtp sender tries to send xr dlrr packet");
}

////////////////////////////////////////////////////////////////////////////////////

void RtcpContextForSend::onRtcp(RtcpHeader *rtcp) {
    switch ((RtcpType)rtcp->pt) {
    case RtcpType::RTCP_RR: {
        auto rtcp_rr = (RtcpRR *)rtcp;
        for (auto item : rtcp_rr->getItemList()) {
            if (!item->last_sr_stamp) {
                continue;
            }
            auto it = _sender_report_ntp.find(item->last_sr_stamp);
            if (it == _sender_report_ntp.end()) {
                continue;
            }
            // Timestamp increment between sending sr and receiving rr
            auto ms_inc = getCurrentMillisecond() - it->second;
            // Delay of the rtp receiver replying to the rr packet after receiving the sr packet, converted to milliseconds
            auto delay_ms = (uint64_t)item->delay_since_last_sr * 1000 / 65536;
            auto rtt = (int)(ms_inc - delay_ms);
            if (rtt >= 0) {
                // RTT cannot be less than 0
                _rtt[item->ssrc] = rtt;
                // InfoL << "ssrc:" << item->ssrc << ",rtt:" << rtt;
            }
        }
        break;
    }
    case RtcpType::RTCP_XR: {
        auto rtcp_xr = (RtcpXRRRTR *)rtcp;
        if (rtcp_xr->bt == 4) {
            _xr_xrrtr_recv_last_rr[rtcp_xr->ssrc]
                = ((rtcp_xr->ntpmsw & 0xFFFF) << 16) | ((rtcp_xr->ntplsw >> 16) & 0xFFFF);
            _xr_rrtr_recv_sys_stamp[rtcp_xr->ssrc] = getCurrentMillisecond();
        } else if (rtcp_xr->bt == 5) {
            TraceL << "for sender not recive dlrr";
        } else {
            TraceL << "not support xr bt " << rtcp_xr->bt;
        }
        break;
    }
    default:
        break;
    }
}

uint32_t RtcpContextForSend::getRtt(uint32_t ssrc) const {
    auto it = _rtt.find(ssrc);
    if (it == _rtt.end()) {
        return 0;
    }
    return it->second;
}

Buffer::Ptr RtcpContextForSend::createRtcpSR(uint32_t rtcp_ssrc) {
    auto rtcp = RtcpSR::create(0);
    rtcp->setNtpStamp(_last_ntp_stamp_ms);
    rtcp->rtpts = htonl(_last_rtp_stamp);
    rtcp->ssrc = htonl(rtcp_ssrc);
    rtcp->packet_count = htonl((uint32_t)_packets);
    rtcp->octet_count = htonl((uint32_t)_bytes);

    // Record the last sent sender report information for subsequent RTT statistics
    auto last_sr_lsr = ((ntohl(rtcp->ntpmsw) & 0xFFFF) << 16) | ((ntohl(rtcp->ntplsw) >> 16) & 0xFFFF);
    _sender_report_ntp[last_sr_lsr] = getCurrentMillisecond();
    if (_sender_report_ntp.size() >= 5) {
        // Delete the earliest sr rtcp
        _sender_report_ntp.erase(_sender_report_ntp.begin());
    }

    return RtcpHeader::toBuffer(std::move(rtcp));
}

toolkit::Buffer::Ptr RtcpContextForSend::createRtcpXRDLRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc) {
    auto rtcp = RtcpXRDLRR::create(1);
    rtcp->bt = 5;
    rtcp->reserved = 0;
    rtcp->block_length = htons(3);
    rtcp->ssrc = htonl(rtcp_ssrc);
    rtcp->items.ssrc = htonl(rtp_ssrc);

    if (_xr_xrrtr_recv_last_rr.find(rtp_ssrc) == _xr_xrrtr_recv_last_rr.end()) {
        rtcp->items.lrr = 0;
        WarnL;
    } else {
        rtcp->items.lrr = htonl(_xr_xrrtr_recv_last_rr[rtp_ssrc]);
    }

    if (_xr_rrtr_recv_sys_stamp.find(rtp_ssrc) == _xr_rrtr_recv_sys_stamp.end()) {
        rtcp->items.dlrr = 0;
        WarnL;
    } else {
        // now - Last SR time, in milliseconds
        auto delay = getCurrentMillisecond() - _xr_rrtr_recv_sys_stamp[rtp_ssrc];
        // in units of 1/65536 seconds
        auto dlsr = (uint32_t)(delay / 1000.0f * 65536);
        rtcp->items.dlrr = htonl(dlsr);
    }
    return RtcpHeader::toBuffer(std::move(rtcp));
}

////////////////////////////////////////////////////////////////////////////////////

void RtcpContextForRecv::onRtp(
    uint16_t seq, uint32_t stamp, uint64_t ntp_stamp_ms, uint32_t sample_rate, size_t bytes) {
    {
        // The receiver performs complex statistical calculations
        auto sys_stamp = getCurrentMillisecond();
        if (_last_rtp_sys_stamp) {
            // Calculate the timestamp jitter value
            double diff = double(
                (int64_t(sys_stamp) - int64_t(_last_rtp_sys_stamp)) * (sample_rate / double(1000.0))
                - (int64_t(stamp) - int64_t(_last_rtp_stamp)));
            if (diff < 0) {
                diff = -diff;
            }
            // Jitter unit is the number of samples
            _jitter += (diff - _jitter) / 16.0;
        } else {
            _jitter = 0;
        }

        if (_last_rtp_seq > 0xFF00 && seq < 0xFF && (!_seq_cycles || _packets - _last_cycle_packets > 0x1FFF)) {
            // Last seq is greater than 0xFF00 and this seq is less than 0xFF,
            // and no loopback occurs or the interval between the last loopback is greater than 0x1FFF packets, then it is considered a loopback
            ++_seq_cycles;
            _last_cycle_packets = _packets;
            _seq_max = seq;
        } else if (seq > _seq_max) {
            // Maximum seq before this loopback
            _seq_max = seq;
        }

        if (!_seq_base) {
            // Record the seq of the first rtp
            _seq_base = seq;
        } else if (!_seq_cycles && seq < _seq_base) {
            // If no loopback occurs, then take the latest seq as the base seq
            _seq_base = seq;
        }

        _last_rtp_seq = seq;
        _last_rtp_sys_stamp = sys_stamp;
    }
    RtcpContext::onRtp(seq, stamp, ntp_stamp_ms, sample_rate, bytes);
}

void RtcpContextForRecv::onRtcp(RtcpHeader *rtcp) {
    switch ((RtcpType)rtcp->pt) {
    case RtcpType::RTCP_SR: {
        auto rtcp_sr = (RtcpSR *)rtcp;
        /**
         last SR timestamp (LSR): 32 bits
          The middle 32 bits out of 64 in the NTP timestamp (as explained in
          Section 4) received as part of the most recent RTCP sender report
          (SR) packet from source SSRC_n.  If no SR has been received yet,
          the field is set to zero.
         */
        _last_sr_lsr = ((rtcp_sr->ntpmsw & 0xFFFF) << 16) | ((rtcp_sr->ntplsw >> 16) & 0xFFFF);
        _last_sr_ntp_sys = getCurrentMillisecond();
        break;
    }
    default:
        break;
    }
}

size_t RtcpContextForRecv::getExpectedPackets() const {
    return (_seq_cycles << 16) + _seq_max - _seq_base + 1;
}

size_t RtcpContextForRecv::getExpectedPacketsInterval() {
    auto expected = getExpectedPackets();
    auto ret = expected - _last_expected;
    _last_expected = expected;
    return ret;
}

size_t RtcpContextForRecv::getLost() {
    return getExpectedPackets() - _packets;
}

size_t RtcpContextForRecv::getLostInterval() {
    auto lost = getLost();
    auto ret = lost - _last_lost;
    _last_lost = lost;
    return ret;
}

Buffer::Ptr RtcpContextForRecv::createRtcpRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc) {
    auto rtcp = RtcpRR::create(1);
    rtcp->ssrc = htonl(rtcp_ssrc);

    ReportItem *item = (ReportItem *)&rtcp->items;
    item->ssrc = htonl(rtp_ssrc);

    uint8_t fraction = 0;
    auto expected_interval = getExpectedPacketsInterval();
    if (expected_interval) {
        fraction = uint8_t(getLostInterval() << 8 / expected_interval);
    }

    item->fraction = fraction;
    item->cumulative = htonl(uint32_t(getLost())) >> 8;
    item->seq_cycles = htons(_seq_cycles);
    item->seq_max = htons(_seq_max);
    item->jitter = htonl(uint32_t(_jitter));
    item->last_sr_stamp = htonl(_last_sr_lsr);

    // now - Last SR time, in milliseconds
    auto delay = getCurrentMillisecond() - _last_sr_ntp_sys;
    // in units of 1/65536 seconds
    auto dlsr = (uint32_t)(delay / 1000.0f * 65536);
    item->delay_since_last_sr = htonl(_last_sr_lsr ? dlsr : 0);
    return RtcpHeader::toBuffer(rtcp);
}

} // namespace mediakit