#ifndef ZLMEDIAKIT_G711_H
#define ZLMEDIAKIT_G711_H

#include "Extension/Frame.h"
#include "Extension/Track.h"

namespace mediakit{

/**
 * G711 audio channel
 */
class G711Track : public AudioTrackImp{
public:
    using Ptr = std::shared_ptr<G711Track>;
    G711Track(CodecId codecId, int sample_rate, int channels, int sample_bit) : AudioTrackImp(codecId, 8000, 1, 16) {}

private:
    Sdp::Ptr getSdp(uint8_t payload_type) const override;
    Track::Ptr clone() const override;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_G711_H