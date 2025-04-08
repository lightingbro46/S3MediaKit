#ifndef S3MEDIAKIT_G711RTP_H
#define S3MEDIAKIT_G711RTP_H

#include "Rtsp/RtpCodec.h"
#include "Extension/Frame.h"
#include "Extension/CommonRtp.h"

namespace mediakit {

/**
 * G711 rtp encoding class
 */
class G711RtpEncoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<G711RtpEncoder>;

    /**
     * Constructor
     * @param sample_rate audio sample rate
     * @param channels Number of channels
     * @param sample_bit audio sample bits
     */
    G711RtpEncoder(int sample_rate = 8000, int channels = 1, int sample_bit = 16);

    /**
     * Input frame data and encode it into rtp
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    void setOpt(int opt, const toolkit::Any &param) override;

private:
    int _channels;
    int _sample_rate;
    int _sample_bit;

    uint32_t _pkt_dur_ms = 20;
    uint32_t _pkt_bytes = 0;
    toolkit::BufferLikeString _buffer;
};

}//namespace mediakit
#endif //S3MEDIAKIT_G711RTP_H
