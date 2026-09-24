#include "LiveAudioSource.h"
#include <cstring>

using namespace toolkit;

namespace mediakit {

LiveAudioSource::LiveAudioSource(const MediaTuple &tuple, uint32_t sampleRate, uint8_t channels, size_t maxQueuedFrames)
    : BackchannelMediaSource(tuple)
    , _frameSamples((size_t)(sampleRate * 20 / 1000))
    , _maxQueuedFrames(maxQueuedFrames) {
    _sampleRate = sampleRate;
    _channels = channels;
}

LiveAudioSource::~LiveAudioSource() {
    std::lock_guard<std::mutex> lock(_mutex);
    _queue.clear();
    _pending.clear();
}

void LiveAudioSource::pushFrame(const int16_t *data, size_t numSamples) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_closed) return;

    _pending.insert(_pending.end(), data, data + numSamples);

    size_t offset = 0;
    while (_pending.size() - offset >= _frameSamples) {
        _queue.emplace_back(_pending.begin() + offset, _pending.begin() + offset + _frameSamples);
        offset += _frameSamples;

        const auto& chunk = _queue.back();
        onWrite(BackchannelFrame::fromPcm(chunk), true);
    }
    _pending.erase(_pending.begin(), _pending.begin() + offset);

    while (_queue.size() > _maxQueuedFrames) {
        _queue.pop_front();
    }
    _cv.notify_one();
}

void LiveAudioSource::markClosed() {
    std::lock_guard<std::mutex> lock(_mutex);
    _closed = true;
    _cv.notify_all();
}

} // namespace mediakit
