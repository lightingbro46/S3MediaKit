#ifndef S3MEDIAKIT_H264RTPCODEC_H
#define S3MEDIAKIT_H264RTPCODEC_H

#include "H264.h"
// for DtsGenerator
#include "Common/Stamp.h"
#include "Rtsp/RtpCodec.h"

namespace mediakit {

/**
 * h264 rtp decoder class
 * Demultiplex h264-Frame from h264 over rtsp-rtp
 * rfc3984
 */
class H264RtpDecoder : public RtpCodec{
public:
    using Ptr = std::shared_ptr<H264RtpDecoder>;

    H264RtpDecoder();

    /**
     * Input 264 rtp packet
     * @param rtp rtp packet
     * @param key_pos This parameter is ignored
     */
    bool inputRtp(const RtpPacket::Ptr &rtp, bool key_pos = true) override;

private:
    bool singleFrame(const RtpPacket::Ptr &rtp, const uint8_t *ptr, ssize_t size, uint64_t stamp);
    bool unpackStapA(const RtpPacket::Ptr &rtp, const uint8_t *ptr, ssize_t size, uint64_t stamp);
    bool mergeFu(const RtpPacket::Ptr &rtp, const uint8_t *ptr, ssize_t size, uint64_t stamp, uint16_t seq);

    bool decodeRtp(const RtpPacket::Ptr &rtp);
    H264Frame::Ptr obtainFrame();
    void outputFrame(const RtpPacket::Ptr &rtp, const H264Frame::Ptr &frame);

private:
    bool _is_gop = false;
    bool _gop_dropped = false;
    bool _fu_dropped = true;
    uint16_t _last_seq = 0;
    H264Frame::Ptr _frame;
    DtsGenerator _dts_generator;
};

/**
 * 264 rtp packaging class
 */
class H264RtpEncoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<H264RtpEncoder>;

    /**
     * Input 264 frame
     * @param frame Frame data, required
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Flush all frame buffers in the output
     */
    void flush() override;

private:
    void insertConfigFrame(uint64_t pts);
    bool inputFrame_l(const Frame::Ptr &frame, bool is_mark);
    void packRtp(const char *data, size_t len, uint64_t pts, bool is_mark, bool gop_pos);
    void packRtpFu(const char *data, size_t len, uint64_t pts, bool is_mark, bool gop_pos);
    void packRtpStapA(const char *data, size_t len, uint64_t pts, bool is_mark, bool gop_pos);
    void packRtpSingleNalu(const char *data, size_t len, uint64_t pts, bool is_mark, bool gop_pos);
    void packRtpSmallFrame(const char *data, size_t len, uint64_t pts, bool is_mark, bool gop_pos);

private:
    Frame::Ptr _sps;
    Frame::Ptr _pps;
    Frame::Ptr _last_frame;
};

}//namespace mediakit

#endif //S3MEDIAKIT_H264RTPCODEC_H
