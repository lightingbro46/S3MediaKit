#ifndef S3MEDIAKIT_PSENCODER_H
#define S3MEDIAKIT_PSENCODER_H

#if defined(ENABLE_RTPPROXY)

#include "Record/MPEG.h"
#include "Common/MediaSink.h"

namespace mediakit {

class CommonRtpEncoder;

class PSEncoderImp : public MpegMuxer {
public:
    /**
     * Create a psh or ts rtp encoder
     * @param ssrc rtp's ssrc
     * @param payload_type rtp's pt
     * @param ps_or_ts true: ps, false: ts
     */
    PSEncoderImp(uint32_t ssrc, uint8_t payload_type = 96, bool ps_or_ts = true);
    ~PSEncoderImp() override;

protected:
    // Callback after rtp packaging
    virtual void onRTP(toolkit::Buffer::Ptr rtp, bool is_key = false) = 0;

protected:
    void onWrite(std::shared_ptr<toolkit::Buffer> buffer, uint64_t stamp, bool key_pos) override;

private:
    std::shared_ptr<CommonRtpEncoder> _rtp_encoder;
};

}//namespace mediakit

#endif //ENABLE_RTPPROXY
#endif //S3MEDIAKIT_PSENCODER_H
