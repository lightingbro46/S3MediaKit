#ifndef BACKCHANNEL_AUDIO_RESAMPLER_H
#define BACKCHANNEL_AUDIO_RESAMPLER_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace mediakit {

/**
 * Resamples PCM16 audio using a basic linear interpolation algorithm and
 * converts the channel count if required.
 *
 * Supports arbitrary channel conversion (upmix/downmix) while preserving
 * interleaved PCM16 format. For stereo-to-mono conversion, the left and right
 * channels are averaged.
 *
 * @param input Pointer to the input PCM16 samples.
 * @param inputSamples Number of input samples (across all channels).
 * @param inputSampleRate Input sample rate in Hz.
 * @param inputChannels Number of input channels.
 * @param outputSampleRate Desired output sample rate in Hz.
 * @param outputChannels Desired number of output channels.
 * @return Resampled and channel-converted PCM16 audio samples.
 *
 * @note This implementation prioritizes simplicity over audio quality.
 * @todo Replace with libspeexdsp or libswresample (FFmpeg) if higher-quality
 *       resampling is required.
 */
std::vector<int16_t> resampleLinear(const int16_t* input, size_t inputSamples,
                                     int inputSampleRate, int inputChannels,
                                     int outputSampleRate, int outputChannels);

} // namespace mediakit

#endif // BACKCHANNEL_AUDIO_RESAMPLER_H
