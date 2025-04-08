#ifndef SRC_RTMP_RTMPDEMUXER_H_
#define SRC_RTMP_RTMPDEMUXER_H_

#include <functional>
#include <unordered_map>
#include "Rtmp/amf.h"
#include "Rtmp/Rtmp.h"
#include "Common/MediaSink.h"
#include "RtmpCodec.h"

namespace mediakit {

class RtmpDemuxer : public Demuxer {
public:
    using Ptr = std::shared_ptr<RtmpDemuxer>;

    static size_t trackCount(const AMFValue &metadata);

    bool loadMetaData(const AMFValue &metadata);

    /**
     * Start demultiplexing
     * @param pkt rtmp packet
     */
    void inputRtmp(const RtmpPacket::Ptr &pkt);

    /**
     * Get the total duration of the program
     * @return Total duration of the program, in seconds
     */
    float getDuration() const;

private:
    void makeVideoTrack(const AMFValue &val, int bit_rate);
    void makeVideoTrack(const Track::Ptr &val, int bit_rate);
    void makeAudioTrack(const AMFValue &val, int sample_rate, int channels, int sample_bit, int bit_rate);

private:
    bool _try_get_video_track = false;
    bool _try_get_audio_track = false;
    float _duration = 0;
    AudioTrack::Ptr _audio_track;
    VideoTrack::Ptr _video_track;
    RtmpCodec::Ptr _audio_rtmp_decoder;
    RtmpCodec::Ptr _video_rtmp_decoder;
};

} /* namespace mediakit */

#endif /* SRC_RTMP_RTMPDEMUXER_H_ */
