#ifndef S3MEDIAKIT_VP8RTPCODEC_H
#define S3MEDIAKIT_VP8RTPCODEC_H

#include "VP8.h"
// for DtsGenerator
#include "Common/Stamp.h"
#include "Rtsp/RtpCodec.h"

namespace mediakit {

/**
 * vp8 rtp decoding class
 * Demultiplex vp8 over rtsp-rtp out of VP8Frame
 */
class VP8RtpDecoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<VP8RtpDecoder>;

    VP8RtpDecoder();

    /**
     * Input vp8 rtp packet
     * @param rtp rtp packet
     * @param key_pos This parameter is ignored
     */
    bool inputRtp(const RtpPacket::Ptr &rtp, bool key_pos = true) override;

private:
    bool decodeRtp(const RtpPacket::Ptr &rtp);
    void outputFrame(const RtpPacket::Ptr &rtp);
    void obtainFrame();

private:
    bool _gop_dropped = false;
    bool _frame_drop = true;
    uint16_t _last_seq = 0;
    VP8Frame::Ptr _frame;
    DtsGenerator _dts_generator;
};

/**
 * vp8 rtp packing class
 */
class VP8RtpEncoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<VP8RtpEncoder>;

    bool inputFrame(const Frame::Ptr &frame) override;

private:
    uint16_t _pic_id = 0;
};

}//namespace mediakit

#endif //S3MEDIAKIT_VP8RTPCODEC_H
