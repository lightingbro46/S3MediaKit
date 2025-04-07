#ifndef ZLMEDIAKIT_MP3_H
#define ZLMEDIAKIT_MP3_H

#include "Extension/Frame.h"
#include "Extension/Track.h"

namespace mediakit {

/**
 * MP3 audio channel
 */
class MP3Track : public AudioTrackImp{
public:
    using Ptr = std::shared_ptr<MP3Track>;
    MP3Track(int sample_rate, int channels) : AudioTrackImp(CodecMP3,sample_rate,channels,16){}

private:
    Sdp::Ptr getSdp(uint8_t payload_type) const override;
    Track::Ptr clone() const override;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_MP3_H