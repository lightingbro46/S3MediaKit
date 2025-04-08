#ifndef S3MEDIAKIT_OPUS_H
#define S3MEDIAKIT_OPUS_H

#include "Extension/Frame.h"
#include "Extension/Track.h"

namespace mediakit {

/**
 * Opus frame audio channel
 */
class OpusTrack : public AudioTrackImp{
public:
    using Ptr = std::shared_ptr<OpusTrack>;
    OpusTrack() : AudioTrackImp(CodecOpus,48000,2,16){}

private:
    // Clone this Track
    Track::Ptr clone() const override {
        return std::make_shared<OpusTrack>(*this);
    }
    // Generate sdp
    Sdp::Ptr getSdp(uint8_t payload_type) const override ;
};

}//namespace mediakit
#endif //S3MEDIAKIT_OPUS_H
