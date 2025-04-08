#ifndef S3MEDIAKIT_RTMPCODEC_H
#define S3MEDIAKIT_RTMPCODEC_H

#include "Rtmp/Rtmp.h"
#include "Extension/Frame.h"
#include "Util/RingBuffer.h"

namespace mediakit{

class RtmpRing {
public:
    using Ptr = std::shared_ptr<RtmpRing>;
    using RingType = toolkit::RingBuffer<RtmpPacket::Ptr>;

    virtual ~RtmpRing() = default;

    /**
     * Set rtmp ring buffer
     */
    void setRtmpRing(const RingType::Ptr &ring) {
        _ring = ring;
    }

    /**
     * Input rtmp packet
     * @param rtmp rtmp packet
     */
    virtual void inputRtmp(const RtmpPacket::Ptr &rtmp) {
        if (_ring) {
            _ring->write(rtmp, rtmp->isVideoKeyFrame());
        }
    }

protected:
    RingType::Ptr _ring;
};

class RtmpCodec : public RtmpRing, public FrameWriterInterface {
public:
    using Ptr = std::shared_ptr<RtmpCodec>;
    RtmpCodec(Track::Ptr track) { _track = std::move(track); }

    virtual void makeConfigPacket() {}

    bool inputFrame(const Frame::Ptr &frame) override { return _track->inputFrame(frame); }

    const Track::Ptr &getTrack() const { return _track; }

private:
    Track::Ptr _track;
};


}//namespace mediakit
#endif //S3MEDIAKIT_RTMPCODEC_H
