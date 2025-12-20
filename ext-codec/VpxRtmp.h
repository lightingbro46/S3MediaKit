#ifndef S3MEDIAKIT_VPX_RTMPCODEC_H
#define S3MEDIAKIT_VPX_RTMPCODEC_H

#include "Rtmp/RtmpCodec.h"
#include "Extension/Track.h"

namespace mediakit {
/**
 * Rtmp decoding class
 * Demultiplex Vpx over rtmp out of VpxFrame
 */
class VpxRtmpDecoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<VpxRtmpDecoder>;

    VpxRtmpDecoder(const Track::Ptr &track) : RtmpCodec(track) {}

    void inputRtmp(const RtmpPacket::Ptr &rtmp) override;

protected:
    void outputFrame(const char *data, size_t size, uint32_t dts, uint32_t pts);

protected:
    RtmpPacketInfo _info;
};

/**
 * Rtmp packing class
 */
class VpxRtmpEncoder : public RtmpCodec {
    bool _enhanced = false;
public:
    using Ptr = std::shared_ptr<VpxRtmpEncoder>;

    VpxRtmpEncoder(const Track::Ptr &track);

    bool inputFrame(const Frame::Ptr &frame) override;

    void makeConfigPacket() override;
};

} // namespace mediakit

#endif // S3MEDIAKIT_VPX_RTMPCODEC_H
