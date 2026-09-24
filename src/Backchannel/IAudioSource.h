#ifndef BACKCHANNEL_I_AUDIO_SOURCE_H
#define BACKCHANNEL_I_AUDIO_SOURCE_H

#include <cstdint>
#include <cstddef>

namespace mediakit {

class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    /**
    * Returns the sample rate of the audio source.
    *
    * @return Audio sample rate in Hz.
    */
    virtual uint32_t sampleRate() const = 0;

    /**
    * Returns the number of audio channels.
    *
    * @return Number of channels (e.g. 1 for mono, 2 for stereo).
    */
    virtual uint8_t channels() const = 0;

protected:
    uint32_t _sampleRate;
    uint8_t _channels;
};

} // namespace mediakit

#endif // BACKCHANNEL_I_AUDIO_SOURCE_H
