#if defined(ENABLE_RTPPROXY)

#include "PSEncoder.h"
#include "Common/config.h"
#include "Extension/CommonRtp.h"
#include "Rtsp/RtspMuxer.h"

using namespace toolkit;

namespace mediakit{

PSEncoderImp::PSEncoderImp(uint32_t ssrc, uint8_t payload_type, bool ps_or_ts) : MpegMuxer(ps_or_ts) {
    GET_CONFIG(uint32_t, s_video_mtu, Rtp::kVideoMtuSize);
    _rtp_encoder = std::make_shared<CommonRtpEncoder>();
    auto video_mtu = s_video_mtu;
    if (!ps_or_ts) {
        // Ensure the ts rtp payload length is a multiple of 188
        video_mtu = RtpPacket::kRtpHeaderSize + (s_video_mtu - (s_video_mtu % 188));
        if (video_mtu > s_video_mtu) {
            video_mtu -= 188;
        }
    }
    _rtp_encoder->setRtpInfo(ssrc, video_mtu, 90000, payload_type);
    auto ring = std::make_shared<RtpRing::RingType>();
    ring->setDelegate(std::make_shared<RingDelegateHelper>([this](RtpPacket::Ptr rtp, bool is_key) { onRTP(std::move(rtp), is_key); }));
    _rtp_encoder->setRtpRing(std::move(ring));
    InfoL << this << " " << ssrc;
}

PSEncoderImp::~PSEncoderImp() {
    InfoL << this;
}

void PSEncoderImp::onWrite(std::shared_ptr<Buffer> buffer, uint64_t stamp, bool key_pos) {
    if (!buffer) {
        return;
    }
    _rtp_encoder->inputFrame(std::make_shared<FrameFromPtr>(CodecH264/*Used only as video*/, buffer->data(), buffer->size(), stamp, stamp, 0, key_pos));
}

}//namespace mediakit

#endif//defined(ENABLE_RTPPROXY)
