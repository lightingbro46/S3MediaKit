#ifndef BACKCHANNEL_FILE_AUDIO_SOURCE_H
#define BACKCHANNEL_FILE_AUDIO_SOURCE_H

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "IAudioSource.h"
#include "BackchannelMediaSource.h"
#include "Poller/Timer.h"

namespace mediakit {

class FileAudioSource : public IAudioSource, public BackchannelMediaSource {
public:
    using Ptr = std::shared_ptr<FileAudioSource>;
    using OnFinished = std::function<void()>;

    explicit FileAudioSource(const MediaTuple &tuple, const std::string &filePath, uint32_t sampleRate = 8000);
    ~FileAudioSource() override;

    uint32_t sampleRate() const override { return _sampleRate; }
    uint8_t channels() const override { return _channels; }

    bool start(std::string &errMsg);

    void stopProducing() override;

    void setOnFinished(OnFinished cb) { _onFinished = std::move(cb); }

private:
    bool openPipe();
    void closePipe();
    bool tick();

private:
    const std::string _filePath;
    const size_t _frameSamples;

    FILE *_pipe{nullptr};
    toolkit::Timer::Ptr _timer;
    bool _running{false};
    OnFinished _onFinished;
};

} // namespace mediakit

#endif // BACKCHANNEL_FILE_AUDIO_SOURCE_H