#ifndef S3MEDIAKIT_VP9RTPCODEC_H
#define S3MEDIAKIT_VP9RTPCODEC_H

#include "VP9.h"
// for DtsGenerator
#include "Common/Stamp.h"
#include "Rtsp/RtpCodec.h"

namespace mediakit {

/**
 * VP9 rtp decoding class
 * Demultiplex VP9 over rtsp-rtp out of VP9Frame
 */
class VP9RtpDecoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<VP9RtpDecoder>;

    VP9RtpDecoder();

    /**
     * Enter VP9 rtp package
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
    VP9Frame::Ptr _frame;
    DtsGenerator _dts_generator;
};

/**
 * VP9 rtp packing class
 */
class VP9RtpEncoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<VP9RtpEncoder>;

    bool inputFrame(const Frame::Ptr &frame) override;
private:
    uint16_t _pic_id = 0;
};

}//namespace mediakit

#endif //S3MEDIAKIT_VP9RTPCODEC_H
