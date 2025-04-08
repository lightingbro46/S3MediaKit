#ifndef S3MEDIAKIT_H264RTMPCODEC_H
#define S3MEDIAKIT_H264RTMPCODEC_H

#include "H264.h"
#include "Rtmp/RtmpCodec.h"
#include "Extension/Track.h"

namespace mediakit {
/**
 * h264 Rtmp decoder class
 * Demultiplex h264-Frame from h264 over rtmp
 */
class H264RtmpDecoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<H264RtmpDecoder>;

    H264RtmpDecoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * Input 264 Rtmp package
     * @param rtmp Rtmp package
     */
    void inputRtmp(const RtmpPacket::Ptr &rtmp) override;

private:
    void outputFrame(const char *data, size_t len, uint32_t dts, uint32_t pts);
    void splitFrame(const uint8_t *data, size_t size, uint32_t dts, uint32_t pts);
};

/**
 * 264 Rtmp packaging class
 */
class H264RtmpEncoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<H264RtmpEncoder>;

    /**
     * Constructor, track can be empty, in which case sps pps is input when inputFrame
     * If track is not empty and contains sps pps information,
     * then sps pps can be omitted when inputFrame
     * @param track
     */
    H264RtmpEncoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * Input 264 frame, sps pps can be omitted
     * @param frame Frame data
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Flush all frame cache output
     */
    void flush() override;

    /**
     * Generate config package
     */
    void makeConfigPacket() override;

private:
    RtmpPacket::Ptr _rtmp_packet;
    FrameMerger _merger { FrameMerger::mp4_nal_size };
};

}//namespace mediakit

#endif //S3MEDIAKIT_H264RTMPCODEC_H
