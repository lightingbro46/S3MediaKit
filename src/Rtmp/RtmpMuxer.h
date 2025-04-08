#ifndef S3MEDIAKIT_RTMPMUXER_H
#define S3MEDIAKIT_RTMPMUXER_H

#include "Rtmp/Rtmp.h"
#include "Extension/Frame.h"
#include "Common/MediaSink.h"
#include "RtmpCodec.h"

namespace mediakit{

class RtmpMuxer : public MediaSinkInterface {
public:
    using Ptr = std::shared_ptr<RtmpMuxer>;

    /**
     * Constructor
     */
    RtmpMuxer(const TitleMeta::Ptr &title);

    /**
     * Get the complete SDP string
     * @return SDP string
     */
    const AMFValue &getMetadata() const ;

    /**
     * Get the rtmp ring buffer
     * @return
     */
    RtmpRing::RingType::Ptr getRtmpRing() const;

    /**
     * Add a ready state track
     */
    bool addTrack(const Track::Ptr & track) override;

    /**
     * Write frame data
     * @param frame frame
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

    /**
     * Generate config package
     */
     void makeConfigPacket();

private:
    bool _track_existed[2] = { false, false };

    AMFValue _metadata;
    RtmpRing::RingType::Ptr _rtmp_ring;
    std::unordered_map<int, RtmpCodec::Ptr> _encoders;
};


} /* namespace mediakit */

#endif //S3MEDIAKIT_RTMPMUXER_H
