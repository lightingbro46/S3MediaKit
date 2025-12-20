#ifndef SDLAUDIOMIXER_SDLAUDIODEVICE_H_
#define SDLAUDIOMIXER_SDLAUDIODEVICE_H_

#include <mutex>
#include <memory>
#include <stdexcept>
#include <unordered_set>

#define DEFAULT_SAMPLERATE 48000
#define DEFAULT_FORMAT AUDIO_S16
#define DEFAULT_CHANNEL 2
#define DEFAULT_SAMPLES 2048


class AudioSRC;

//This object mainly implements SDL mixing and playback
class SDLAudioDevice : public std::enable_shared_from_this<SDLAudioDevice>{
public:
    using Ptr = std::shared_ptr<SDLAudioDevice>;

    ~SDLAudioDevice();
    static SDLAudioDevice &Instance();

    void addChannel(AudioSRC *chn);
    void delChannel(AudioSRC *chn);

private:
    SDLAudioDevice();
    void onReqPCM(char *stream, int len);

private:
    SDL_AudioDeviceID _device;
    std::shared_ptr<char> _play_buf;
    SDL_AudioSpec _audio_config;
    std::recursive_mutex _channel_mtx;
    std::unordered_set<AudioSRC *> _channels;
};

#endif /* SDLAUDIOMIXER_SDLAUDIODEVICE_H_ */
