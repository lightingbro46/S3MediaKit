#ifndef BACKCHANNEL_LIVE_AUDIO_SOURCE_H
#define BACKCHANNEL_LIVE_AUDIO_SOURCE_H

#include "IAudioSource.h"
#include "BackchannelMediaSource.h"
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace mediakit {

class LiveAudioSource : public IAudioSource, public BackchannelMediaSource {
public:
    using Ptr = std::shared_ptr<LiveAudioSource>;

    explicit LiveAudioSource(const MediaTuple &tuple, uint32_t sampleRate, uint8_t channels, size_t maxQueuedFrames = 25);
    ~LiveAudioSource() override;

    void pushFrame(const int16_t* data, size_t numSamples);

    void markClosed();

    uint32_t sampleRate() const override { return _sampleRate; }

    uint8_t channels() const override { return _channels; }

private:
    size_t _frameSamples;
    size_t _maxQueuedFrames;
    std::deque<std::vector<int16_t>> _queue;
    std::vector<int16_t> _pending;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _closed = false;
};

} // namespace mediakit

#endif // BACKCHANNEL_LIVE_AUDIO_SOURCE_H
