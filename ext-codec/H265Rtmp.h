#ifndef ZLMEDIAKIT_H265RTMPCODEC_H
#define ZLMEDIAKIT_H265RTMPCODEC_H

#include "H265.h"
#include "Rtmp/RtmpCodec.h"
#include "Extension/Track.h"

namespace mediakit {
/**
 * h265 Rtmp decoder class
 * Demultiplex h265-Frame from h265 over rtmp
 */
class H265RtmpDecoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<H265RtmpDecoder>;

    H265RtmpDecoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * Input 265 Rtmp packet
     * @param rtmp Rtmp packet
     */
    void inputRtmp(const RtmpPacket::Ptr &rtmp) override;

protected:
    void outputFrame(const char *data, size_t size, uint32_t dts, uint32_t pts);
    void splitFrame(const uint8_t *data, size_t size, uint32_t dts, uint32_t pts);

protected:
    RtmpPacketInfo _info;
};

/**
 * 265 Rtmp packaging class
 */
class H265RtmpEncoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<H265RtmpEncoder>;

    /**
     * Constructor, track can be empty, in which case sps pps is input when inputFrame
     * If track is not empty and contains sps pps information,
     * Then sps pps can be omitted when inputFrame
     * @param track
     */
    H265RtmpEncoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * Input 265 frame, sps pps can be omitted
     * @param frame Frame data
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Flush all frame cache output
     */
    void flush() override;

    /**
     * Generate config packet
     */
    void makeConfigPacket() override;

private:
    RtmpPacket::Ptr _rtmp_packet;
    FrameMerger _merger { FrameMerger::mp4_nal_size };
};

} // namespace mediakit

#endif // ZLMEDIAKIT_H265RTMPCODEC_H
