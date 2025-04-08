#ifndef S3MEDIAKIT_RTSPMUXER_H
#define S3MEDIAKIT_RTSPMUXER_H

#include "Extension/Frame.h"
#include "Common/MediaSink.h"
#include "Common/Stamp.h"
#include "RtpCodec.h"

namespace mediakit{

class RingDelegateHelper : public toolkit::RingDelegate<RtpPacket::Ptr> {
public:
    using onRtp = std::function<void(RtpPacket::Ptr in, bool is_key)> ;

    RingDelegateHelper(onRtp on_rtp) {
        _on_rtp = std::move(on_rtp);
    }

    void onWrite(RtpPacket::Ptr in, bool is_key) override {
        _on_rtp(std::move(in), is_key);
    }

private:
    onRtp _on_rtp;
};

/**
 * RTSP generator
*/
class RtspMuxer : public MediaSinkInterface {
public:
    using Ptr = std::shared_ptr<RtspMuxer>;

    /**
     * Constructor
     */
    RtspMuxer(const TitleSdp::Ptr &title = nullptr);

    /**
     * Get the complete SDP string
     * @return SDP string
     */
    std::string getSdp() ;

    /**
     * Get the RTP ring buffer
     * @return
     */
    RtpRing::RingType::Ptr getRtpRing() const;

    /**
     * Add a ready state track
     */
    bool addTrack(const Track::Ptr & track) override;

    /**
     * Write frame data
     * @param frame Frame
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Flush all frame buffers
     */
    void flush() override;

    /**
     * Reset all tracks
     */
    void resetTracks() override ;

private:
    void onRtp(RtpPacket::Ptr in, bool is_key);
    void trySyncTrack();

private:
    bool _live = true;
    bool _track_existed[2] = { false, false };

    uint8_t _index {0};
    uint64_t _ntp_stamp_start;
    std::string _sdp;

    struct TrackInfo {
        Stamp stamp;
        uint32_t rtp_stamp { 0 };
        uint64_t ntp_stamp { 0 };
        RtpCodec::Ptr encoder;
    };

    std::unordered_map<int, TrackInfo> _tracks;
    RtpRing::RingType::Ptr _rtpRing;
    RtpRing::RingType::Ptr _rtpInterceptor;
};


} /* namespace mediakit */

#endif //S3MEDIAKIT_RTSPMUXER_H
