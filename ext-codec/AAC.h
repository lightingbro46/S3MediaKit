#ifndef ZLMEDIAKIT_AAC_H
#define ZLMEDIAKIT_AAC_H

#include "Extension/Frame.h"
#include "Extension/Track.h"
#define ADTS_HEADER_LEN 7

namespace mediakit{

/**
 * AAC audio channel
 */
class AACTrack : public AudioTrack {
public:
    using Ptr = std::shared_ptr<AACTrack>;

    AACTrack() = default;

    /**
     * Construct object through AAC extra data
     */
    AACTrack(const std::string &aac_cfg);

    bool ready() const override;
    CodecId getCodecId() const override;
    int getAudioChannel() const override;
    int getAudioSampleRate() const override;
    int getAudioSampleBit() const override;
    bool inputFrame(const Frame::Ptr &frame) override;
    toolkit::Buffer::Ptr getExtraData() const override;
    void setExtraData(const uint8_t *data, size_t size) override;
    bool update() override;

private:
    Sdp::Ptr getSdp(uint8_t payload_type) const override;
    Track::Ptr clone() const override;
    bool inputFrame_l(const Frame::Ptr &frame);

private:
    std::string _cfg;
    int _channel = 0;
    int _sampleRate = 0;
    int _sampleBit = 16;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_AAC_H