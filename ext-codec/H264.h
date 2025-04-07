#ifndef ZLMEDIAKIT_H264_H
#define ZLMEDIAKIT_H264_H

#include "Extension/Frame.h"
#include "Extension/Track.h"

#define H264_TYPE(v) ((uint8_t)(v) & 0x1F)

namespace mediakit{

void splitH264(const char *ptr, size_t len, size_t prefix, const std::function<void(const char *, size_t, size_t)> &cb);
size_t prefixSize(const char *ptr, size_t len);

template<typename Parent>
class H264FrameHelper : public Parent{
public:
    friend class FrameImp;
    friend class toolkit::ResourcePool_l<H264FrameHelper>;
    using Ptr = std::shared_ptr<H264FrameHelper>;

    enum {
        NAL_IDR = 5,
        NAL_SEI = 6,
        NAL_SPS = 7,
        NAL_PPS = 8,
        NAL_AUD = 9,
        NAL_B_P = 1,
    };

    template<typename ...ARGS>
    H264FrameHelper(ARGS &&...args): Parent(std::forward<ARGS>(args)...) {
        this->_codec_id = CodecH264;
    }

    bool keyFrame() const override {
        auto nal_ptr = (uint8_t *) this->data() + this->prefixSize();
        return H264_TYPE(*nal_ptr) == NAL_IDR && decodeAble();
    }

    bool configFrame() const override {
        auto nal_ptr = (uint8_t *) this->data() + this->prefixSize();
        switch (H264_TYPE(*nal_ptr)) {
            case NAL_SPS:
            case NAL_PPS: return true;
            default: return false;
        }
    }

    bool dropAble() const override {
        auto nal_ptr = (uint8_t *) this->data() + this->prefixSize();
        switch (H264_TYPE(*nal_ptr)) {
            case NAL_SEI:
            case NAL_AUD: return true;
            default: return false;
        }
    }

    bool decodeAble() const override {
        auto nal_ptr = (uint8_t *) this->data() + this->prefixSize();
        auto type = H264_TYPE(*nal_ptr);
        // // In the case of multiple slices, first_mb_in_slice indicates the start of a frame
        return type >= NAL_B_P && type <= NAL_IDR && (nal_ptr[1] & 0x80);
    }
};

/**
 * 264 frame class
 */
using H264Frame = H264FrameHelper<FrameImp>;

/**
 * H264 class that prevents memory copying
 * Users can quickly wrap a pointer into a Frame class without copying using this type
 */
using H264FrameNoCacheAble = H264FrameHelper<FrameFromPtr>;

/**
 * 264 video channel
 */
class H264Track : public VideoTrack {
public:
    using Ptr = std::shared_ptr<H264Track>;

    /**
     * Construct a media of h264 type without specifying sps pps
     * Get sps pps in the subsequent inputFrame
     */
    H264Track() = default;

    /**
     * Construct a media of h264 type
     * @param sps sps frame data
     * @param pps pps frame data
     * @param sps_prefix_len 264 header length, can be 3 or 4 bytes, generally 0x00 00 00 01
     * @param pps_prefix_len 264 header length, can be 3 or 4 bytes, generally 0x00 00 00 01
     */
    H264Track(const std::string &sps, const std::string &pps, int sps_prefix_len = 4, int pps_prefix_len = 4);

    bool ready() const override;
    CodecId getCodecId() const override;
    int getVideoHeight() const override;
    int getVideoWidth() const override;
    float getVideoFps() const override;
    bool inputFrame(const Frame::Ptr &frame) override;
    toolkit::Buffer::Ptr getExtraData() const override;
    void setExtraData(const uint8_t *data, size_t size) override;
    bool update() override;
    std::vector<Frame::Ptr> getConfigFrames() const override;

private:
    Sdp::Ptr getSdp(uint8_t payload_type) const override;
    Track::Ptr clone() const override;
    bool inputFrame_l(const Frame::Ptr &frame);
    void insertConfigFrame(const Frame::Ptr &frame);

    bool latestIsConfigFrame();

private:
    bool _latest_is_pps = false;
    bool _latest_is_sps = false;
    int _width = 0;
    int _height = 0;
    float _fps = 0;
    std::string _sps;
    std::string _pps;
};

template <typename FrameType>
Frame::Ptr createConfigFrame(const std::string &data, uint64_t dts, int index) {
    auto frame = FrameImp::create<FrameType>();
    frame->_prefix_size = 4;
    frame->_buffer.assign("\x00\x00\x00\x01", 4);
    frame->_buffer.append(data);
    frame->_dts = dts;
    frame->setIndex(index);
    return frame;
}

}//namespace mediakit

#endif //ZLMEDIAKIT_H264_H
