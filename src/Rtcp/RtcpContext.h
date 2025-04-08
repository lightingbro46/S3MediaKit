#ifndef S3MEDIAKIT_RTCPCONTEXT_H
#define S3MEDIAKIT_RTCPCONTEXT_H

#include "Rtcp.h"
#include <stddef.h>
#include <stdint.h>

namespace mediakit {

class RtcpContext {
public:
    using Ptr = std::shared_ptr<RtcpContext>;
    virtual ~RtcpContext() = default;

    /**
     * Called when outputting or inputting rtp
     * @param seq rtp's seq
     * @param stamp rtp's timestamp, unit is sample number (not millisecond)
     * @param ntp_stamp_ms ntp timestamp
     * @param rtp rtp timestamp sampling rate, video is generally 90000, audio is generally sampling rate
     * @param bytes rtp data length
     */
    virtual void onRtp(uint16_t seq, uint32_t stamp, uint64_t ntp_stamp_ms, uint32_t sample_rate, size_t bytes);

    /**
     * Input sr rtcp packet
     * @param rtcp input an rtcp
     */
    virtual void onRtcp(RtcpHeader *rtcp) = 0;

    /**
     * Calculate the total number of lost packets
     */
    virtual size_t getLost();

    /**
     * Return the number of rtp that should be received
     */
    virtual size_t getExpectedPackets() const;

    /**
     * Create SR rtcp packet
     * @param rtcp_ssrc rtcp's ssrc
     * @return rtcp packet
     */
    virtual toolkit::Buffer::Ptr createRtcpSR(uint32_t rtcp_ssrc);

    /**
     * @brief Create xr's dlrr packet, used by receiver to estimate rtt
     * @return toolkit::Buffer::Ptr
     */
    virtual toolkit::Buffer::Ptr createRtcpXRDLRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc);

    /**
     * Create RR rtcp packet
     * @param rtcp_ssrc rtcp's ssrc
     * @param rtp_ssrc rtp's ssrc
     * @return rtcp packet
     */
    virtual toolkit::Buffer::Ptr createRtcpRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc);

    /**
     * Number of packets that should be received between the last result and the current result
     */
    virtual size_t getExpectedPacketsInterval();

    /**
     * Number of lost packets between the last result and the current result
     */
    virtual size_t getLostInterval();

protected:
    // Number of bytes of rtp received or sent
    size_t _bytes = 0;
    // Number of rtp received or sent
    size_t _packets = 0;
    // Last rtp timestamp, milliseconds
    uint32_t _last_rtp_stamp = 0;
    uint64_t _last_ntp_stamp_ms = 0;
};

class RtcpContextForSend : public RtcpContext {
public:
    toolkit::Buffer::Ptr createRtcpSR(uint32_t rtcp_ssrc) override;

    void onRtcp(RtcpHeader *rtcp) override;

    toolkit::Buffer::Ptr createRtcpXRDLRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc) override;

    /**
     * Get rtt
     * @param ssrc rtp ssrc
     * @return rtt, unit is millisecond
     */
    uint32_t getRtt(uint32_t ssrc) const;

private:
    std::map<uint32_t /*ssrc*/, uint32_t /*rtt*/> _rtt;
    std::map<uint32_t /*last_sr_lsr*/, uint64_t /*ntp stamp*/> _sender_report_ntp;

    std::map<uint32_t /*ssrc*/, uint64_t /*xr rrtr sys stamp*/> _xr_rrtr_recv_sys_stamp;
    std::map<uint32_t /*ssrc*/, uint32_t /*last rr */> _xr_xrrtr_recv_last_rr;
};

class RtcpContextForRecv : public RtcpContext {
public:
    void onRtp(uint16_t seq, uint32_t stamp, uint64_t ntp_stamp_ms, uint32_t sample_rate, size_t bytes) override;
    toolkit::Buffer::Ptr createRtcpRR(uint32_t rtcp_ssrc, uint32_t rtp_ssrc) override;
    size_t getExpectedPackets() const override;
    size_t getExpectedPacketsInterval() override;
    size_t getLost() override;
    size_t getLostInterval() override;
    void onRtcp(RtcpHeader *rtcp) override;

private:
    // Timestamp jitter value
    double _jitter = 0;
    // The value of the first seq
    uint16_t _seq_base = 0;
    // Maximum rtp seq
    uint16_t _seq_max = 0;
    // Rtp loopback times
    uint16_t _seq_cycles = 0;
    // Number of rtp packets recorded when the last loopback occurred
    size_t _last_cycle_packets = 0;
    // Last seq
    uint16_t _last_rtp_seq = 0;
    // Last rtp system timestamp (milliseconds) used for jitter statistics
    uint64_t _last_rtp_sys_stamp = 0;
    // Last total number of lost packets counted
    size_t _last_lost = 0;
    // Last total number of rtp packets that should be received counted
    size_t _last_expected = 0;
    // Last SR timestamp calculated when the last SR packet was received
    uint32_t _last_sr_lsr = 0;
    // System timestamp when the last SR was received, unit is millisecond
    uint64_t _last_sr_ntp_sys = 0;
};

} // namespace mediakit
#endif // S3MEDIAKIT_RTCPCONTEXT_H
