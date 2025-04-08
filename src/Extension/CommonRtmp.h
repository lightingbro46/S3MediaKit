#ifndef S3MEDIAKIT_COMMONRTMP_H
#define S3MEDIAKIT_COMMONRTMP_H

#include "Frame.h"
#include "Rtmp/RtmpCodec.h"

namespace mediakit{

/**
 * Generic rtmp decoder class
 */
class CommonRtmpDecoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<CommonRtmpDecoder>;

    /**
     * Constructor
     */
    CommonRtmpDecoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * Input Rtmp and decode
     * @param rtmp Rtmp data packet
     */
    void inputRtmp(const RtmpPacket::Ptr &rtmp) override;
};

/**
 * Generic rtmp encoder class
 */
class CommonRtmpEncoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<CommonRtmpEncoder>;

    CommonRtmpEncoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * Input frame data
     */
    bool inputFrame(const Frame::Ptr &frame) override;

private:
    uint8_t _audio_flv_flags { 0 };
};

}//namespace mediakit
#endif //S3MEDIAKIT_COMMONRTMP_H
