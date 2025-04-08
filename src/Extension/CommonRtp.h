#ifndef S3MEDIAKIT_COMMONRTP_H
#define S3MEDIAKIT_COMMONRTP_H

#include "Frame.h"
#include "Rtsp/RtpCodec.h"

namespace mediakit{

/**
 * Generic rtp decoder class
 */
class CommonRtpDecoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr <CommonRtpDecoder>;

    /**
     * Constructor
     * @param codec codec id
     * @param max_frame_size maximum allowed frame size
     */
    CommonRtpDecoder(CodecId codec, size_t max_frame_size = 2 * 1024);

    /**
     * Input rtp and decode
     * @param rtp rtp data packet
     * @param key_pos This parameter is internally forced to false, please ignore it
     */
    bool inputRtp(const RtpPacket::Ptr &rtp, bool key_pos = false) override;

private:
    void obtainFrame();

private:
    bool _drop_flag = false;
    uint16_t _last_seq = 0;
    uint64_t _last_stamp = 0;
    size_t _max_frame_size;
    CodecId _codec;
    FrameImp::Ptr _frame;
};

/**
 * Generic rtp encoder class
 */
class CommonRtpEncoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr <CommonRtpEncoder>;

    /**
     * Input frame data and encode into rtp
     */
    bool inputFrame(const Frame::Ptr &frame) override;
};

}//namespace mediakit
#endif //S3MEDIAKIT_COMMONRTP_H
