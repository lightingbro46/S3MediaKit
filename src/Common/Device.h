#ifndef DEVICE_DEVICE_H_
#define DEVICE_DEVICE_H_

#include <memory>
#include <string>
#include <functional>
#include "Util/TimeTicker.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {

class H264Encoder;
class AACEncoder;

class VideoInfo {
public:
    CodecId codecId = CodecH264;
    int iWidth;
    int iHeight;
    float iFrameRate;
    int iBitRate = 2 * 1024 * 1024;
};

class AudioInfo {
public:
    CodecId codecId = CodecAAC;
    int iChannel;
    int iSampleBit;
    int iSampleRate;
};

/**
 * Wrapper class for MultiMediaSourceMuxer, making it easier for beginners to use.
 */
class DevChannel  : public MultiMediaSourceMuxer{
public:
    using Ptr = std::shared_ptr<DevChannel>;

    // fDuration<=0 for live streaming, otherwise for on-demand
    DevChannel(const MediaTuple& tuple, float duration = 0, const ProtocolOption &option = ProtocolOption())
        : MultiMediaSourceMuxer(tuple, duration, option) {}

    /**
     * Initialize the video Track
     * Equivalent to MultiMediaSourceMuxer::addTrack(VideoTrack::Ptr );
     * @param info Video related information
     */
    bool initVideo(const VideoInfo &info);

    /**
     * Initialize the audio Track
     * Equivalent to MultiMediaSourceMuxer::addTrack(AudioTrack::Ptr );
     * @param info Audio related information
     */
    bool initAudio(const AudioInfo &info);

    /**
     * Input 264 frame
     * @param data 264 single frame data pointer
     * @param len Data pointer length
     * @param dts Decode timestamp, in milliseconds; If it is 0, the timestamp will be generated automatically internally
     * @param pts Play timestamp, in milliseconds; If it is 0, it will be assigned to dts internally
     */
    bool inputH264(const char *data, int len, uint64_t dts, uint64_t pts = 0);

    /**
     * Input 265 frame
     * @param data 265 single frame data pointer
     * @param len Data pointer length
     * @param dts Decode timestamp, in milliseconds; If it is 0, the timestamp will be generated automatically internally
     * @param pts Play timestamp, in milliseconds; If it is 0, it will be assigned to dts internally
     */
    bool inputH265(const char *data, int len, uint64_t dts, uint64_t pts = 0);

    /**
     * Input aac frame
     * @param data_without_adts aac frame without adts header
     * @param len Frame data length
     * @param dts Timestamp, in milliseconds
     * @param adts_header adts header
     */
    bool inputAAC(const char *data_without_adts, int len, uint64_t dts, const char *adts_header);

    /**
     * Input OPUS/G711 audio frame
     * @param data Audio frame
     * @param len Frame data length
     * @param dts Timestamp, in milliseconds
     */
    bool inputAudio(const char *data, int len, uint64_t dts);

    /**
     * Input yuv420p video frame, encoding will be completed internally and inputH264 method will be called
     * @param yuv yuv420p data pointer
     * @param linesize yuv420p data linesize
     * @param cts Capture timestamp, in milliseconds
     */
    bool inputYUV(char *yuv[3], int linesize[3], uint64_t cts);

    /**
     * Input pcm data, encoding will be completed internally and inputAAC method will be called
     * @param data pcm data pointer, int16 integer
     * @param len pcm data length
     * @param cts Capture timestamp, in milliseconds
     */
    bool inputPCM(char *data, int len, uint64_t cts);

    // // Override base class methods to ensure thread safety ////
    bool inputFrame(const Frame::Ptr &frame) override;
    bool addTrack(const Track::Ptr & track) override;
    void addTrackCompleted() override;

private:
    MediaOriginType getOriginType(MediaSource &sender) const override;

private:
    std::shared_ptr<H264Encoder> _pH264Enc;
    std::shared_ptr<AACEncoder> _pAacEnc;
    std::shared_ptr<VideoInfo> _video;
    std::shared_ptr<AudioInfo> _audio;
    toolkit::SmoothTicker _aTicker[2];
};

} /* namespace mediakit */

#endif /* DEVICE_DEVICE_H_ */
