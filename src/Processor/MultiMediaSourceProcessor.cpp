#if defined(ENABLE_FFMPEG)

#include "MultiMediaSourceProcessor.h"
#include "Common/MultiMediaSourceMuxer.h"

using namespace std;

namespace mediakit {

MultiMediaSourceProcessor::MultiMediaSourceProcessor(const MediaTuple &tuple, const ProtocolOption &option, const toolkit::EventPoller::Ptr &poller)
    : _tuple(tuple), _option(option),
      _poller(poller ? poller : toolkit::EventPollerPool::Instance().getPoller(false)) {
    // Keep source lifecycle callbacks on the source poller, but never run
    // transcode or motion processing there. Both operations may perform
    // expensive image/codec work and would otherwise starve RTP handling.
    _transcode_poller = toolkit::EventPollerPool::Instance().getPoller(false);
#if defined(ENABLE_MOTION)
    _motion_poller = toolkit::EventPollerPool::Instance().getPoller(false);
#endif // ENABLE_MOTION
    _source_on_demand = option.auto_close;
#if defined(ENABLE_MOTION)
    if (option.enable_motion || option.enable_transcode) {
        _ring = std::make_shared<RingType>(512, nullptr, 1);
    }
#else
    if (option.enable_transcode) {
        _ring = std::make_shared<RingType>(512, nullptr, 1);
    }
#endif
#if defined(ENABLE_MOTION)
    if (option.enable_motion) {
        GET_CONFIG(int, interval_ms, Motion::kIntervalMS);
        GET_CONFIG(bool, use_y_channel, Motion::kUseYChannel);
        _motion = std::make_shared<MotionProcessor>(tuple, option.roi_mask, option.record_motion, option.motion_record_stream_id, interval_ms, use_y_channel);
        // Create the live MJPEG muxer only when enabled (either always-on or demand mode).
        // motion_demand=true  → source becomes active only when a viewer connects.
        // motion_demand=false → source is always active once motion detection starts.
        _mjpeg_muxer = std::make_shared<MotionMjpegMediaSourceMuxer>(tuple, option);
    }
#else
    if (option.enable_motion) {
        WarnL << "Motion detection is not enabled. Please turn on the ENABLE_MOTION macro when compiling to use this feature.";
    }
#endif

    if (option.enable_transcode) {
        TranscodeProcessor::Config cfg;
        cfg.codec = CodecH264;
        cfg.width = option.transcode_width;
        cfg.height = option.transcode_height;
        cfg.fps = option.transcode_fps > 0 ? option.transcode_fps : 25;
        cfg.bitrate = option.transcode_bitrate;
        cfg.gop = option.transcode_gop;
        cfg.demand = option.transcode_demand;
        cfg.overlay_image = option.transcode_overlay_image;
        cfg.overlay_x = option.transcode_overlay_x;
        cfg.overlay_y = option.transcode_overlay_y;
        _transcodes[cfg.stream_suffix] = createTranscode(cfg);
    }
}

TranscodeProcessor::Ptr MultiMediaSourceProcessor::createTranscode(const TranscodeProcessor::Config &cfg) {
    auto transcode = std::make_shared<TranscodeProcessor>(_tuple, _option, cfg, _transcode_poller);
    for (const auto &track : _audio_tracks) {
        transcode->addAudioTrack(track);
    }
    if (_tracks_completed) {
        transcode->finalizeTracks();
    }
    return transcode;
}

MediaSource::Ptr MultiMediaSourceProcessor::ensureTranscode(const TranscodeProcessor::Config &cfg) {
    const auto key = cfg.stream_suffix;
    std::lock_guard<std::mutex> lock(_mtx);
    auto it = _transcodes.find(key);
    if (it == _transcodes.end()) {
        auto transcode = createTranscode(cfg);
        transcode->setListener(shared_from_this());

        std::weak_ptr<MultiMediaSourceProcessor> weak_self = shared_from_this();
        transcode->setOnClosed([weak_self, key](const TranscodeProcessor::Ptr &closed) {
            auto self = weak_self.lock();
            if (self) {
                self->removeTranscode(key, closed);
            }
        });
        it = _transcodes.emplace(key, transcode).first;
        attachTranscodeReader(key, transcode);
    }
    return MediaSource::find(cfg.output_schema, _tuple.vhost, _tuple.app, _tuple.stream + cfg.stream_suffix);
}

void MultiMediaSourceProcessor::setListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);
#if defined(ENABLE_MOTION)
    if (_motion) {
        _motion->setListener(shared_from_this());
    }
    if (_mjpeg_muxer) {
        _mjpeg_muxer->setListener(shared_from_this());
    }
    if (_motion) {
        std::lock_guard<std::mutex> lock(_mtx);
        attachMotionReader();
    }
#endif // ENABLE_MOTION
    std::vector<std::pair<std::string, TranscodeProcessor::Ptr>> transcodes;
    {
        std::lock_guard<std::mutex> lock(_mtx);
        for (const auto &entry : _transcodes) {
            transcodes.emplace_back(entry.first, entry.second);
        }
    }
    for (const auto &entry : transcodes) {
        auto key = entry.first;
        auto transcode = entry.second;
        transcode->setListener(shared_from_this());

        std::weak_ptr<MultiMediaSourceProcessor> weak_self = shared_from_this();
        transcode->setOnClosed([weak_self, key](const TranscodeProcessor::Ptr &closed) {
            auto self = weak_self.lock();
            if (self) {
                self->removeTranscode(key, closed);
            }
        });
        {
            std::lock_guard<std::mutex> lock(_mtx);
            attachTranscodeReader(key, transcode);
        }
    }
}

void MultiMediaSourceProcessor::removeTranscode(const std::string &key, const TranscodeProcessor::Ptr &transcode) {
    std::function<void()> on_idle;
    {
        std::lock_guard<std::mutex> lock(_mtx);
        auto it = _transcodes.find(key);
        if (it == _transcodes.end() || it->second != transcode) {
            WarnL << "Skip remove transcode: instance mismatch, key=" << key;
            return;
        }
        DebugL << "Remove closed transcode stream: " << key;
        _transcode_readers.erase(key);
        _transcodes.erase(it);
    }
    if (canClose()) {
        on_idle = _on_idle;
    }
    if (on_idle) {
        on_idle();
    }
}

void MultiMediaSourceProcessor::attachTranscodeReader(const std::string &key, const TranscodeProcessor::Ptr &transcode) {
    if (!transcode || _transcode_readers.find(key) != _transcode_readers.end()) {
        return;
    }
    if (!_ring) {
        // One decoded GOP is enough to prime a newly-created transcode.
        // This ring is independent from the output muxer's GOP cache.
        _ring = std::make_shared<RingType>(512, nullptr, 1);
    }

    auto ring = _ring;
    auto poller = _transcode_poller;
    std::weak_ptr<TranscodeProcessor> weak_transcode = transcode;
    RingType::RingReader::Ptr reader;
    poller->sync([&]() {
        // Do not replay the cached GOP synchronously while the source poller
        // is handling the HTTP request/RTP stream. The encoder will start from
        // the next decoded frame on the dedicated transcode poller.
        reader = ring->attach(poller, false);
        reader->setReadCB([weak_transcode](const DecodedFrame &input) {
            auto strong_transcode = weak_transcode.lock();
            if (!strong_transcode) {
                return;
            }
            if (input.type == DecodedFrame::Video) {
                strong_transcode->inputVideoFrame(input.video);
            } else if (input.audio) {
                strong_transcode->inputAudioFrame(input.audio);
            }
        });
    });
    _transcode_readers.emplace(key, reader);
}

void MultiMediaSourceProcessor::attachMotionReader() {
    if (!_motion || _motion_reader) {
        return;
    }
    if (!_ring) {
        _ring = std::make_shared<RingType>(512, nullptr, 1);
    }

    auto ring = _ring;
    auto poller = _motion_poller;
    std::weak_ptr<MotionProcessor> weak_motion = _motion;
    poller->sync([&]() {
        _motion_reader = ring->attach(poller, false);
        _motion_reader->setReadCB([weak_motion](const DecodedFrame &input) {
            auto motion = weak_motion.lock();
            if (motion && input.type == DecodedFrame::Video && input.video) {
                motion->inputFrame(input.video);
            }
        });
    });
}

bool MultiMediaSourceProcessor::addTrack(const Track::Ptr &track) {
    bool ret = MediaSourceDecoder::addTrack(track);
    // Register the original audio track for pass-through muxing into the transcode stream.
    if (track && track->getTrackType() == TrackAudio) {
        _audio_tracks.push_back(track);
    }
    if (track && track->getTrackType() == TrackAudio) {
        for (const auto &transcode : snapshotTranscodes()) {
            transcode->addAudioTrack(track);
        }
    }
    return ret;
}

bool MultiMediaSourceProcessor::inputFrame(const Frame::Ptr &frame) {
    // Base class decodes video frames through onDecode(). Audio is passed through
    // the same input ring so A/V delivery to each transcode stays serialized.
    bool ret = MediaSourceDecoder::inputFrame(frame);
    if (frame && frame->getTrackType() == TrackAudio) {
        RingType::Ptr ring;
        {
            std::lock_guard<std::mutex> lock(_mtx);
            ring = _ring;
        }
        if (ring) {
            DecodedFrame input;
            input.type = DecodedFrame::Audio;
            input.audio = frame;
            ring->write(input, false);
        }
    }
    return ret;
}

void MultiMediaSourceProcessor::addTrackCompleted() {
#if defined(ENABLE_MOTION)
    if (haveVideo() && _motion && _mjpeg_muxer) {
        _motion->setMjpegMuxer(_mjpeg_muxer);
    }
#endif // ENABLE_MOTION
    _tracks_completed = true;
    for (const auto &transcode : snapshotTranscodes()) {
        transcode->finalizeTracks();
    }
}

void MultiMediaSourceProcessor::onDecode(const FFmpegFrame::Ptr &frame) {
    TraceL << "Decoded frame dts: " << frame->get()->pkt_dts << ", pts: " << frame->get()->pts << ", size: " << frame->get()->pkt_size;
    // Dispatch the decoded frame once. Motion and transcode processors consume
    // it through their own ring readers on the processor poller.
    RingType::Ptr ring;
    {
        std::lock_guard<std::mutex> lock(_mtx);
        ring = _ring;
    }
    if (ring && frame) {
        DecodedFrame input;
        input.type = DecodedFrame::Video;
        input.video = frame;
        ring->write(input, frame->get() && frame->get()->key_frame > 0);
    }
}

void MultiMediaSourceProcessor::resetTracks() {
    MediaSourceDecoder::resetTracks();
    _audio_tracks.clear();
    {
        std::lock_guard<std::mutex> lock(_mtx);
#if defined(ENABLE_MOTION)
        _motion_reader.reset();
#endif // ENABLE_MOTION
        _transcode_readers.clear();
        _transcodes.clear();
        if (_ring) {
            _ring->clearCache();
        }
    }
    _tracks_completed = false;
}

std::vector<TranscodeProcessor::Ptr> MultiMediaSourceProcessor::snapshotTranscodes() const {
    std::vector<TranscodeProcessor::Ptr> ret;
    std::lock_guard<std::mutex> lock(_mtx);
    ret.reserve(_transcodes.size());
    for (const auto &entry : _transcodes) {
        ret.emplace_back(entry.second);
    }
    return ret;
}

bool MultiMediaSourceProcessor::isTranscodeEnabled() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return !_transcodes.empty();
}

bool MultiMediaSourceProcessor::isEnabled() {
#if defined(ENABLE_MOTION)
    if (_mjpeg_muxer && _mjpeg_muxer->isEnabled()) {
        return true;
    }
#endif // ENABLE_MOTION
    for (const auto &transcode : snapshotTranscodes()) {
        if (transcode && transcode->isEnabled()) {
            return true;
        }
    }
    return false;
}

bool MultiMediaSourceProcessor::canClose() const {
    if (readerCount() != 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_mtx);
        if (!_transcodes.empty()) {
            return false;
        }
    }
#if defined(ENABLE_MOTION)
    if (_motion && (!_option.motion_demand || _option.record_motion)) {
        return false;
    }
#endif // ENABLE_MOTION
    for (const auto &transcode : snapshotTranscodes()) {
        if (transcode && !transcode->isOnDemand()) {
            return false;
        }
    }
    return true;
}

void MultiMediaSourceProcessor::setOnIdle(const std::function<void()> &callback) {
    _on_idle = callback;
}

void MultiMediaSourceProcessor::onReaderChanged(MediaSource &sender, int size) {
    const int readers = readerCount();
    DebugL << "Derived reader state: " << _tuple.shortUrl() << ", event_size=" << size << ", total_readers=" << readers;

    if (readers > 0) {
        _idle_timer = nullptr;
    } else if (canClose() && !_idle_timer) {
        GET_CONFIG(int, delay_ms, General::kStreamNoneReaderDelayMS);
        std::weak_ptr<MultiMediaSourceProcessor> weak_self = shared_from_this();
        _idle_timer = std::make_shared<toolkit::Timer>(delay_ms / 1000.0f, [weak_self]() {
            auto self = weak_self.lock();
            if (!self || self->readerCount() > 0) {
                return false;
            }
            if (!self->canClose()) {
                return true;
            }
            auto callback = self->_on_idle;
            if (callback) {
                callback();
            }
            return false;
        }, _poller);
    }

    // A live source must not be closed because a derived output became idle.
    // For an on-demand source, preserve the original source lifecycle policy.
    if (_source_on_demand) {
        MediaSourceEventInterceptor::onReaderChanged(sender, readers);
    }
}

bool MultiMediaSourceProcessor::isMotionDetectRunning() {
#if defined(ENABLE_MOTION)
    return !!_motion;
#else
    return false;
#endif // ENABLE_MOTION
}

int MultiMediaSourceProcessor::readerCount() const {
    int totalCount = 0;
#if defined(ENABLE_MOTION)
    totalCount += _mjpeg_muxer ? _mjpeg_muxer->readerCount() : 0;
#endif // ENABLE_MOTION
    for (const auto &transcode : snapshotTranscodes()) {
        totalCount += transcode->totalReaderCount();
    } 
    return totalCount;
}

} // namespace mediakit

#endif // ENABLE_FFMPEG
