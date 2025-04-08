#ifndef S3MEDIAKIT_FACTORY_H
#define S3MEDIAKIT_FACTORY_H

#include <string>
#include "Rtmp/amf.h"
#include "Extension/Track.h"
#include "Extension/Frame.h"
#include "Rtsp/RtpCodec.h"
#include "Rtmp/RtmpCodec.h"
#include "Util/onceToken.h"

#define REGISTER_STATIC_VAR_INNER(var_name, line) var_name##_##line##__
#define REGISTER_STATIC_VAR(var_name, line) REGISTER_STATIC_VAR_INNER(var_name, line)

#define REGISTER_CODEC(plugin) \
extern CodecPlugin plugin;     \
static toolkit::onceToken REGISTER_STATIC_VAR(s_token, __LINE__) ([]() { \
    Factory::registerPlugin(plugin); \
});

namespace mediakit {

struct CodecPlugin {
    CodecId (*getCodec)();
    Track::Ptr (*getTrackByCodecId)(int sample_rate, int channels, int sample_bit);
    Track::Ptr (*getTrackBySdp)(const SdpTrack::Ptr &track);
    RtpCodec::Ptr (*getRtpEncoderByCodecId)(uint8_t pt);
    RtpCodec::Ptr (*getRtpDecoderByCodecId)();
    RtmpCodec::Ptr (*getRtmpEncoderByTrack)(const Track::Ptr &track);
    RtmpCodec::Ptr (*getRtmpDecoderByTrack)(const Track::Ptr &track);
    Frame::Ptr (*getFrameFromPtr)(const char *data, size_t bytes, uint64_t dts, uint64_t pts);
};

class Factory {
public:
    /**
     * Register plugin, not thread-safe
     */
    static void registerPlugin(const CodecPlugin &plugin);

    /**
     * Get track by codec_id
     * @param codecId codec id
     * @param sample_rate sample rate, video is fixed to 90000
     * @param channels number of audio channels
     * @param sample_bit audio sample bit
     */
    static Track::Ptr getTrackByCodecId(CodecId codecId, int sample_rate = 0, int channels = 1, int sample_bit = 16);

    // //////////////////////////////rtsp related//////////////////////////////////
    /**
     * Generate Track object based on sdp
     */
    static Track::Ptr getTrackBySdp(const SdpTrack::Ptr &track);

    /**
     * Generate specific Track object based on Track abstracted from c api
     */
    static Track::Ptr getTrackByAbstractTrack(const Track::Ptr& track);

    /**
     * Generate rtp encoder based on codec id
     * @param codec_id codec id
     * @param pt rtp payload type
     */
    static RtpCodec::Ptr getRtpEncoderByCodecId(CodecId codec_id, uint8_t pt);

    /**
     * Generate Rtp unpacker based on Track
     */
    static RtpCodec::Ptr getRtpDecoderByCodecId(CodecId codec);


    // //////////////////////////////rtmp related//////////////////////////////////

    /**
     * Get the corresponding video Track based on the amf object
     * @param amf the value of videocodecid in rtmp metadata
     */
    static Track::Ptr getVideoTrackByAmf(const AMFValue &amf);

    /**
     * Get the corresponding audio Track based on the amf object
     * @param amf the value of audiocodecid in rtmp metadata
     */
    static Track::Ptr getAudioTrackByAmf(const AMFValue& amf, int sample_rate, int channels, int sample_bit);

    /**
     * Get the Rtmp encoder based on Track
     * @param track media description object
     */
    static RtmpCodec::Ptr getRtmpEncoderByTrack(const Track::Ptr &track);

    /**
     * Get the Rtmp decoder based on Track
     * @param track media description object
     */
    static RtmpCodec::Ptr getRtmpDecoderByTrack(const Track::Ptr &track);

    /**
     * Get the rtmp codec description based on codecId
     */
    static AMFValue getAmfByCodecId(CodecId codecId);

    static Frame::Ptr getFrameFromPtr(CodecId codec, const char *data, size_t size, uint64_t dts, uint64_t pts);
    static Frame::Ptr getFrameFromBuffer(CodecId codec, toolkit::Buffer::Ptr data, uint64_t dts, uint64_t pts);
};

}//namespace mediakit

#endif //S3MEDIAKIT_FACTORY_H
