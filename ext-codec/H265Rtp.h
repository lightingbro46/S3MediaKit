#ifndef S3MEDIAKIT_H265RTPCODEC_H
#define S3MEDIAKIT_H265RTPCODEC_H

#include "H265.h"
#include "Rtsp/RtpCodec.h"
// for DtsGenerator
#include "Common/Stamp.h"

namespace mediakit {

/**
 * h265 rtp decoder class
 * Demultiplex h265-Frame from h265 over rtsp-rtp
 * 《Draft (H265-over-RTP) draft-ietf-payload-rtp-h265-07.pdf》
 */
class H265RtpDecoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<H265RtpDecoder>;

    H265RtpDecoder();

    /**
     * Input 265 rtp packet
     * @param rtp rtp packet
     * @param key_pos This parameter is ignored
     */
    bool inputRtp(const RtpPacket::Ptr &rtp, bool key_pos = true) override;

private:
    bool unpackAp(const RtpPacket::Ptr &rtp, const uint8_t *ptr, ssize_t size, uint64_t stamp);
    bool mergeFu(const RtpPacket::Ptr &rtp, const uint8_t *ptr, ssize_t size, uint64_t stamp, uint16_t seq);
    bool singleFrame(const RtpPacket::Ptr &rtp, const uint8_t *ptr, ssize_t size, uint64_t stamp);

    bool decodeRtp(const RtpPacket::Ptr &rtp);
    H265Frame::Ptr obtainFrame();
    void outputFrame(const RtpPacket::Ptr &rtp, const H265Frame::Ptr &frame);

private:
    bool _is_gop = false;
    bool _using_donl_field = false;
    bool _gop_dropped = false;
    bool _fu_dropped = true;
    uint16_t _last_seq = 0;
    H265Frame::Ptr _frame;
    DtsGenerator _dts_generator;
};

/**
 * 265 rtp packer class
 */
class H265RtpEncoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<H265RtpEncoder>;

    /**
     * Input 265 frame
     * @param frame Frame data, required
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Flush all frame cache in output
     */
    void flush() override;

private:
    void packRtp(const char *ptr, size_t len, uint64_t pts, bool is_mark, bool gop_pos);
    void packRtpFu(const char *ptr, size_t len, uint64_t pts, bool is_mark, bool gop_pos);
    void insertConfigFrame(uint64_t pts);
    bool inputFrame_l(const Frame::Ptr &frame, bool is_mark);
private:
    Frame::Ptr _sps;
    Frame::Ptr _pps;
    Frame::Ptr _vps;
    Frame::Ptr _last_frame;
};

}//namespace mediakit

#endif //S3MEDIAKIT_H265RTPCODEC_H
