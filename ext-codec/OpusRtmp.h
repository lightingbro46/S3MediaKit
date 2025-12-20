#ifndef S3MEDIAKIT_OPUS_RTMPCODEC_H
#define S3MEDIAKIT_OPUS_RTMPCODEC_H

#include "Rtmp/RtmpCodec.h"
#include "Extension/Track.h"

namespace mediakit {
/**
 * Rtmp decoder class
 * Demux Opus over rtmp to OpusFrame
 */
class OpusRtmpDecoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<OpusRtmpDecoder>;

    OpusRtmpDecoder(const Track::Ptr &track) : RtmpCodec(track) {}

    void inputRtmp(const RtmpPacket::Ptr &rtmp) override;

protected:
    void outputFrame(const char *data, size_t size, uint32_t dts, uint32_t pts);
};

/**
 * Rtmp packing class
 */
class OpusRtmpEncoder : public RtmpCodec {
    bool _enhanced = false;
public:
    using Ptr = std::shared_ptr<OpusRtmpEncoder>;

    OpusRtmpEncoder(const Track::Ptr &track);

    bool inputFrame(const Frame::Ptr &frame) override;

    void makeConfigPacket() override;
};

} // namespace mediakit

#endif // S3MEDIAKIT_OPUS_RTMPCODEC_H
