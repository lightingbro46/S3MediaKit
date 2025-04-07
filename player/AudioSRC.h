#ifndef AUDIOSRC_H_
#define AUDIOSRC_H_

#include <memory>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif
#include "SDL2/SDL.h"
#ifdef __cplusplus
}
#endif

#if defined(_WIN32)
#pragma comment(lib,"SDL2.lib")
#endif //defined(_WIN32)

#include "Network/Buffer.h"
#include "SDLAudioDevice.h"

class AudioSRCDelegate {
public:
    virtual ~AudioSRCDelegate() {};
    virtual SDL_AudioFormat getPCMFormat() = 0;
    virtual int getPCMSampleRate() = 0;
    virtual int getPCMChannel() = 0;
    virtual int getPCMData(char *buf, int size) = 0;
};

//This class implements resampling of pcm
class AudioSRC {
public:
    using Ptr = std::shared_ptr<AudioSRC>;
    AudioSRC(AudioSRCDelegate *);
    virtual ~AudioSRC();

    void setEnableMix(bool flag);
    void setOutputAudioConfig(const SDL_AudioSpec &cfg);
    int getPCMData(char *buf, int size);

private:
    bool _enabled = true;
    int _buf_size = 0;
    std::shared_ptr<char> _buf;
    AudioSRCDelegate *_delegate = nullptr;
    toolkit::BufferLikeString _target_buf;
    SDL_AudioCVT _audio_cvt;
};

class AudioPlayer : public AudioSRC, private AudioSRCDelegate{
public:
    AudioPlayer();
    ~AudioPlayer() override;

    void setup(int sample_rate, int channel, SDL_AudioFormat format);
    void playPCM(const char *data, size_t size);

private:
    SDL_AudioFormat getPCMFormat() override;
    int getPCMSampleRate() override;
    int getPCMChannel() override;
    int getPCMData(char *buf, int size) override;

private:
    int _sample_rate, _channel;
    SDL_AudioFormat _format;
    std::mutex _mtx;
    toolkit::BufferLikeString _buffer;
    SDLAudioDevice::Ptr _device;
};

#endif /* AUDIOSRC_H_ */
