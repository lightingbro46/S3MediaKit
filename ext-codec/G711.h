#ifndef S3MEDIAKIT_G711_H
#define S3MEDIAKIT_G711_H

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

    toolkit::Buffer::Ptr getExtraData() const override;
    void setExtraData(const uint8_t *data, size_t size) override;
private:
    Track::Ptr clone() const override { return std::make_shared<G711Track>(*this); }
};

}//namespace mediakit
#endif //S3MEDIAKIT_G711_H