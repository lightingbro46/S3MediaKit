#include "JPEG.h"
#include "JPEGRtp.h"
#include "Rtsp/Rtsp.h"
#include "Util/util.h"
#include "Extension/Factory.h"

using namespace toolkit;

namespace mediakit {

bool JPEGTrack::inputFrame(const Frame::Ptr &frame) {
    if (!ready()) {
        if (_height > 0 && _width > 0) {
            if (_tmp == 0) _tmp = frame->dts();
            else if (frame->dts() > _tmp) _fps = 1000.0 / (frame->dts() - _tmp);
        } else getVideoResolution((uint8_t*)frame->data(), frame->size());
        return false;
    }
    // Follow resolution changes of the camera; the SOF marker sits in the first few hundred bytes.
    getVideoResolution((uint8_t *)frame->data(), frame->size());
    return VideoTrack::inputFrame(frame);
}

void JPEGTrack::getVideoResolution(const uint8_t *buf, int len) {
    // Walk the marker segments from SOI so bytes inside DQT/DHT tables are never taken for a marker.
    int i = 2;
    if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8 /*SOI*/) {
        return;
    }
    while (i + 4 <= len) {
        if (buf[i] != 0xFF) {
            return;
        }
        uint8_t marker = buf[i + 1];
        if (marker == 0xFF) {
            // Fill byte
            ++i;
            continue;
        }
        if (marker == 0xDA /*SOS*/ || marker == 0xD9 /*EOI*/) {
            return;
        }
        int seg_len = buf[i + 2] * 256 + buf[i + 3];
        if (marker >= 0xC0 && marker <= 0xC2 /*SOF0..SOF2*/) {
            if (i + 9 > len) {
                return;
            }
            _height = buf[i + 5] * 256 + buf[i + 6];
            _width = buf[i + 7] * 256 + buf[i + 8];
            return;
        }
        if (seg_len < 2) {
            return;
        }
        i += 2 + seg_len;
    }
}

Sdp::Ptr JPEGTrack::getSdp(uint8_t pt) const {
    return std::make_shared<DefaultSdp>(pt, *this);
}

namespace {

CodecId getCodec() {
    return CodecJPEG;
}

Track::Ptr getTrackByCodecId(int sample_rate, int channels, int sample_bit) {
    return std::make_shared<JPEGTrack>();
}

Track::Ptr getTrackBySdp(const SdpTrack::Ptr &track) {
    return std::make_shared<JPEGTrack>();
}

RtpCodec::Ptr getRtpEncoderByCodecId(uint8_t pt) {
    return std::make_shared<JPEGRtpEncoder>();
}

RtpCodec::Ptr getRtpDecoderByCodecId() {
    return std::make_shared<JPEGRtpDecoder>();
}

RtmpCodec::Ptr getRtmpEncoderByTrack(const Track::Ptr &track) {
    WarnL << "Unsupported jpeg rtmp encoder";
    return nullptr;
}

RtmpCodec::Ptr getRtmpDecoderByTrack(const Track::Ptr &track) {
    WarnL << "Unsupported jpeg rtmp decoder";
    return nullptr;
}

Frame::Ptr getFrameFromPtr(const char *data, size_t bytes, uint64_t dts, uint64_t pts) {
    return std::make_shared<JPEGFrame<FrameFromPtr>>(0, CodecJPEG, (char *)data, bytes, dts, pts);
}

} // namespace

CodecPlugin jpeg_plugin = { getCodec,
                            getTrackByCodecId,
                            getTrackBySdp,
                            getRtpEncoderByCodecId,
                            getRtpDecoderByCodecId,
                            getRtmpEncoderByTrack,
                            getRtmpDecoderByTrack,
                            getFrameFromPtr };

} // namespace mediakit
