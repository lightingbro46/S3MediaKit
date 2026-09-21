#include "AudioResampler.h"
#include <algorithm>
#include <cmath>

namespace mediakit {

// Convert the channel count while preserving interleaved PCM16 format
static std::vector<int16_t> convertChannels(const int16_t* input, size_t inputSamples,
                                            int inputChannels, int outputChannels) {
    if (!input || inputSamples == 0 || inputChannels <= 0 || outputChannels <= 0) {
        return {};
    }

    if (inputChannels == outputChannels) {
        return std::vector<int16_t>(input, input + inputSamples);
    }

    const size_t frames = inputSamples / static_cast<size_t>(inputChannels);
    if (frames == 0) {
        return {};
    }

    std::vector<int16_t> out(frames * static_cast<size_t>(outputChannels));

    if (outputChannels == 1) {
        // Downmix -> Mono
        for (size_t i = 0; i < frames; ++i) {
            int32_t sum = 0;
            for (int c = 0; c < inputChannels; ++c) {
                sum += input[i * inputChannels + c];
            }
            out[i] = static_cast<int16_t>(sum / inputChannels);
        }
        return out;
    }

    if (inputChannels == 1) {
        // Upmix Mono -> N channel
        for (size_t i = 0; i < frames; ++i) {
            int16_t sample = input[i];
            for (int c = 0; c < outputChannels; ++c) {
                out[i * outputChannels + c] = sample;
            }
        }
        return out;
    }

    if (outputChannels < inputChannels) {
        // General downmix
        for (size_t i = 0; i < frames; ++i) {
            for (int oc = 0; oc < outputChannels; ++oc) {
                const int start = oc * inputChannels / outputChannels;
                const int end = std::max(start + 1, (oc + 1) * inputChannels / outputChannels);

                int32_t sum = 0;
                int count = 0;

                for (int c = start; c < end && c < inputChannels; ++c) {
                    sum += input[i * inputChannels + c];
                    ++count;
                }

                out[i * outputChannels + oc] = static_cast<int16_t>(count ? sum / count : 0);
            }
        }
    } else {
        // General upmix
        for (size_t i = 0; i < frames; ++i) {
            for (int oc = 0; oc < outputChannels; ++oc) {
                out[i * outputChannels + oc] = input[i * inputChannels + (oc % inputChannels)];
            }
        }
    }

    return out;
}

std::vector<int16_t> resampleLinear(const int16_t* input, size_t inputSamples,
                                    int inputSampleRate, int inputChannels,
                                    int outputSampleRate, int outputChannels) {
    auto converted = convertChannels(input, inputSamples, inputChannels, outputChannels);
    if (converted.empty()) {
        return {};
    }

    const size_t frames = converted.size() / static_cast<size_t>(outputChannels);

    if (inputSampleRate <= 0 || outputSampleRate <= 0 || inputSampleRate == outputSampleRate) {
        return converted;
    }

    const double ratio = static_cast<double>(inputSampleRate) / outputSampleRate;
    const size_t outFrames = static_cast<size_t>(std::llround(frames / ratio));

    std::vector<int16_t> out(outFrames * static_cast<size_t>(outputChannels));

    for (size_t i = 0; i < outFrames; ++i) {
        const double srcPos = i * ratio;

        size_t idx0 = static_cast<size_t>(srcPos);
        idx0 = std::min(idx0, frames - 1);

        const size_t idx1 = std::min(idx0 + 1, frames - 1);
        const double frac = srcPos - idx0;

        for (int c = 0; c < outputChannels; ++c) {
            const int16_t s0 = converted[idx0 * outputChannels + c];
            const int16_t s1 = converted[idx1 * outputChannels + c];
            out[i * outputChannels + c] = static_cast<int16_t>(s0 * (1.0 - frac) + s1 * frac);
        }
    }

    return out;
}

} // namespace mediakit
