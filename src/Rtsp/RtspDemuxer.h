#ifndef SRC_RTP_RTSPDEMUXER_H_
#define SRC_RTP_RTSPDEMUXER_H_

#include <unordered_map>
#include "Rtsp/RtpCodec.h"
#include "Common/MediaSink.h"

namespace mediakit {

class RtspDemuxer : public Demuxer {
public:
    using Ptr = std::shared_ptr<RtspDemuxer>;

    /**
     * Load sdp
     */
    void loadSdp(const std::string &sdp);

    /**
     * Start demultiplexing
     * @param rtp rtp packet
     * @return true represents the first rtp packet of the i-frame
     */
    bool inputRtp(const RtpPacket::Ptr &rtp);

    /**
     * Get the total duration of the program
     * @return Total duration of the program, in seconds
     */
    float getDuration() const;

private:
    void makeAudioTrack(const SdpTrack::Ptr &audio);
    void makeVideoTrack(const SdpTrack::Ptr &video);
    void loadSdp(const SdpParser &parser);

private:
    float _duration = 0;
    AudioTrack::Ptr _audio_track;
    VideoTrack::Ptr _video_track;
    RtpCodec::Ptr _audio_rtp_decoder;
    RtpCodec::Ptr _video_rtp_decoder;
};

} /* namespace mediakit */

#endif /* SRC_RTP_RTSPDEMUXER_H_ */
