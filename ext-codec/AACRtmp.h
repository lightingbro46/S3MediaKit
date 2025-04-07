#ifndef ZLMEDIAKIT_AACRTMPCODEC_H
#define ZLMEDIAKIT_AACRTMPCODEC_H

#include "AAC.h"
#include "Rtmp/RtmpCodec.h"
#include "Extension/Track.h"

namespace mediakit {
/**
 * aac Rtmp to adts class
 */
class AACRtmpDecoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<AACRtmpDecoder>;

    AACRtmpDecoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * Input Rtmp and decode
     * @param rtmp Rtmp data packet
     */
    void inputRtmp(const RtmpPacket::Ptr &rtmp) override;
};

/**
 * aac adts to Rtmp class
 */
class AACRtmpEncoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<AACRtmpEncoder>;

    /**
     * Constructor, track can be empty, in which case the adts header is input when inputFrame is called
     * If track is not empty and contains adts header related information,
     * then the adts header can be omitted when inputFrame is called
     * @param track
     */
    AACRtmpEncoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * Input aac data, can be without adts header
     * @param frame aac data
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Generate config package
     */
    void makeConfigPacket() override;

private:
    uint8_t _audio_flv_flags {0};
};

}//namespace mediakit

#endif //ZLMEDIAKIT_AACRTMPCODEC_H
