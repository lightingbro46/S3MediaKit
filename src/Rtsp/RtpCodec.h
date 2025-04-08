#ifndef S3MEDIAKIT_RTPCODEC_H
#define S3MEDIAKIT_RTPCODEC_H

#include <memory>
#include "Extension/Frame.h"
#include "Util/RingBuffer.h"
#include "Rtsp/Rtsp.h"

namespace mediakit {

class RtpRing {
public:
    using Ptr = std::shared_ptr<RtpRing>;
    using RingType = toolkit::RingBuffer<RtpPacket::Ptr>;

    virtual ~RtpRing() = default;

    /**
     * Set the RTP ring buffer
     * @param ring
     */
    void setRtpRing(RingType::Ptr ring) {
        _ring = std::move(ring);
    }

    /**
     * Input RTP packet
     * @param rtp RTP packet
     * @param key_pos Whether it is the first RTP packet of the key frame
     * @return Whether it is the first RTP packet of the key frame
     */
    virtual bool inputRtp(const RtpPacket::Ptr &rtp, bool key_pos) {
        if (_ring) {
            _ring->write(rtp, key_pos);
        }
        return key_pos;
    }

protected:
    RingType::Ptr _ring;
};

class RtpInfo {
public:
    using Ptr = std::shared_ptr<RtpInfo>;

    RtpInfo(uint32_t ssrc, size_t mtu_size, uint32_t sample_rate, uint8_t pt, uint8_t interleaved, int track_index) {
        if (ssrc == 0) {
            ssrc = ((uint64_t) this) & 0xFFFFFFFF;
        }
        _pt = pt;
        _ssrc = ssrc;
        _mtu_size = mtu_size;
        _sample_rate = sample_rate;
        _interleaved = interleaved;
        _track_index = track_index;
    }

    // Return the maximum length of the RTP payload
    size_t getMaxSize() const {
        return _mtu_size - RtpPacket::kRtpHeaderSize;
    }

    RtpPacket::Ptr makeRtp(TrackType type,const void *data, size_t len, bool mark, uint64_t stamp);

private:
    uint8_t _pt;
    uint8_t _interleaved;
    uint16_t _seq = 0;
    uint32_t _ssrc;
    uint32_t _sample_rate;
    int _track_index;
    size_t _mtu_size;
};

class RtpCodec : public RtpRing, public FrameDispatcher {
public:
    using Ptr = std::shared_ptr<RtpCodec>;

    void setRtpInfo(uint32_t ssrc, size_t mtu_size, uint32_t sample_rate, uint8_t pt, uint8_t interleaved = 0, int track_index = 0) {
        _rtp_info.reset(new RtpInfo(ssrc, mtu_size, sample_rate, pt, interleaved, track_index));
    }

    RtpInfo &getRtpInfo() { return *_rtp_info; }

    enum {
        RTP_ENCODER_PKT_DUR_MS = 1 // It is mainly used in the g711 rtp packager time length, option_value is int*, option_len is 4
    };
    /**
     * @brief Set the parameters of the RTP packer and unpacker, mainly used for g711 RTP packer, the usage is similar to setsockopt
     * @param opt Set options
     * @param param Set parameters
     */
    virtual void setOpt(int opt, const toolkit::Any &param) {};

private:
    std::unique_ptr<RtpInfo> _rtp_info;
};

}//namespace mediakit


#endif //S3MEDIAKIT_RTPCODEC_H
