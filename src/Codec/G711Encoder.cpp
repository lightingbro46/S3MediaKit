#include "G711Encoder.h"

namespace mediakit {

uint8_t G711Encoder::linearToUlaw(int16_t sampleIn) {
    const int BIAS = 0x84;
    const int CLIP = 32635;

    int sample = sampleIn;
    int sign = 0;
    if (sample < 0) {
        sign = 0x80;
        sample = -sample;
    }
    if (sample > CLIP) sample = CLIP;
    sample += BIAS;

    int exponent = 7;
    int expMask = 0x4000;
    while (exponent > 0 && !(sample & expMask)) {
        exponent -= 1;
        expMask >>= 1;
    }

    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa) & 0xFF);
}

void G711Encoder::pcm16ToUlaw(const int16_t* pcm, size_t numSamples, uint8_t* outUlaw) {
    for (size_t i = 0; i < numSamples; ++i) {
        outUlaw[i] = linearToUlaw(pcm[i]);
    }
}

uint8_t G711Encoder::linearToAlaw(int16_t sampleIn) {
    const int16_t CLIP = 32767;

    int sample = sampleIn;
    int sign = 0;

    if (sample < 0) {
        sign = 0x80;
        sample = -sample;
        if (sample > CLIP)
            sample = CLIP;
    }

    int exponent;
    int mantissa;

    if (sample >= 256) {
        exponent = 7;
        int expMask = 0x4000;
        while ((sample & expMask) == 0 && exponent > 0) {
            exponent--;
            expMask >>= 1;
        }

        mantissa = (sample >> (exponent + 3)) & 0x0F;
        sample = (exponent << 4) | mantissa;
    } else {
        sample >>= 4;
    }

    sample ^= (sign ^ 0x55);

    return static_cast<uint8_t>(sample);
}

void G711Encoder::pcm16ToAlaw(const int16_t *pcm, size_t numSamples, uint8_t *outAlaw) {
    for (size_t i = 0; i < numSamples; ++i) {
        outAlaw[i] = linearToAlaw(pcm[i]);
    }
}

} // namespace mediakit
