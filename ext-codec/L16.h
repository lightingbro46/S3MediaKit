#ifndef ZLMEDIAKIT_L16_H
#define ZLMEDIAKIT_L16_H

#include "Extension/Frame.h"
#include "Extension/Track.h"

namespace mediakit {

/**
 * L16 audio channel
 */
class L16Track : public AudioTrackImp{
public:
    using Ptr = std::shared_ptr<L16Track>;
    L16Track(int sample_rate, int channels) : AudioTrackImp(CodecL16,sample_rate,channels,16){}

private:
    Sdp::Ptr getSdp(uint8_t payload_type) const override;
    Track::Ptr clone() const override;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_L16_H