#ifndef CODEC_G711ENCODER_H_
#define CODEC_G711ENCODER_H_

#include <cstdint>
#include <cstddef>

namespace mediakit {

class G711Encoder {
public:
    // Encode PCM16 -> G.711 μ-law (PCMU)
    static void pcm16ToUlaw(const int16_t* pcm, size_t numSamples, uint8_t* outUlaw);

    // Encode PCM16 -> G.711 A-law (PCMA)
    static void pcm16ToAlaw(const int16_t* pcm, size_t numSamples, uint8_t* outAlaw);

private:
    static uint8_t linearToUlaw(int16_t sample);

    static uint8_t linearToAlaw(int16_t sample);
};

} // namespace mediakit

#endif // CODEC_G711ENCODER_H_
