#ifndef ZLMEDIAKIT_AACRTPCODEC_H
#define ZLMEDIAKIT_AACRTPCODEC_H

#include "AAC.h"
#include "Rtsp/RtpCodec.h"

namespace mediakit {
/**
 * aac rtp to adts class
 */
class AACRtpDecoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<AACRtpDecoder>;

    AACRtpDecoder();

    /**
     * input rtp and decode
     * @param rtp rtp data packet
     * @param key_pos this parameter is internally forced to false, please ignore it
     */
    bool inputRtp(const RtpPacket::Ptr &rtp, bool key_pos = false) override;

private:
    void obtainFrame();
    void flushData();

private:
    uint64_t _last_dts = 0;
    FrameImp::Ptr _frame;
};


/**
 * aac adts to rtp class
 */
class AACRtpEncoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<AACRtpEncoder>;

    /**
     * input aac data, must have dats header
     * @param frame aac data with dats header
     */
    bool inputFrame(const Frame::Ptr &frame) override;

private:
    void outputRtp(const char *data, size_t len, size_t total_len, bool mark, uint64_t stamp);

};

}//namespace mediakit

#endif //ZLMEDIAKIT_AACRTPCODEC_H
