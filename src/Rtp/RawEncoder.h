#ifndef S3MEDIAKIT_RAWENCODER_H
#define S3MEDIAKIT_RAWENCODER_H

#if defined(ENABLE_RTPPROXY)

#include "Common/MediaSink.h"
#include "Rtsp/RtpCodec.h"

namespace mediakit {

class RawEncoderImp : public MediaSinkInterface {
public:
    RawEncoderImp(uint32_t ssrc, uint8_t payload_type = 96, bool send_audio = true);
    ~RawEncoderImp() override;

    /**
     * Add audio and video tracks
     */
    bool addTrack(const Track::Ptr &track) override;

    /**
     * Reset audio and video tracks
     */
    void resetTracks() override;

    /**
     * Input frame data
     */
    bool inputFrame(const Frame::Ptr &frame) override;

protected:
    // Callback after RTP packaging
    virtual void onRTP(toolkit::Buffer::Ptr rtp, bool is_key = false) = 0;

private:
    std::shared_ptr<RtpCodec> createRtpEncoder(const Track::Ptr &track);

private:
    bool _send_audio;
    uint8_t _payload_type;
    uint32_t _ssrc;
    RtpCodec::Ptr _rtp_encoder;
};

} // namespace mediakit

#endif // ENABLE_RTPPROXY
#endif // S3MEDIAKIT_RAWENCODER_H
