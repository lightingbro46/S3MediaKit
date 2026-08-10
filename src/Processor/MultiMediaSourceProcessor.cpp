#if defined(ENABLE_FFMPEG)

#include "MultiMediaSourceProcessor.h"
#include "Common/MultiMediaSourceMuxer.h"

using namespace std;

namespace mediakit {

MultiMediaSourceProcessor::MultiMediaSourceProcessor(const MediaTuple &tuple, const ProtocolOption &option, const toolkit::EventPoller::Ptr &poller)
    : _tuple(tuple), _option(option),
      _poller(poller ? poller : toolkit::EventPollerPool::Instance().getPoller()) {
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
    auto transcode = std::make_shared<TranscodeProcessor>(_tuple, _option, cfg, _poller);
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
    }
}

void MultiMediaSourceProcessor::removeTranscode(const std::string &key, const TranscodeProcessor::Ptr &transcode) {
    std::lock_guard<std::mutex> lock(_mtx);
    auto it = _transcodes.find(key);
    if (it == _transcodes.end() || it->second != transcode) {
        return;
    }
    InfoL << "Remove closed transcode stream: " << key;
    _transcodes.erase(it);
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
    // Base class decodes video frames (dispatched to onDecode). Audio frames have
    // no decoder there, so forward them to the transcode stream as pass-through.
    bool ret = MediaSourceDecoder::inputFrame(frame);
    if (frame && frame->getTrackType() == TrackAudio) {
        for (const auto &transcode : snapshotTranscodes()) {
            transcode->inputAudioFrame(frame);
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
    // Dispatch decoded frame to all tracks
#if defined(ENABLE_MOTION)
    if (_motion) {
        _motion->inputFrame(frame);
    }
#endif // ENABLE_MOTION
    for (const auto &transcode : snapshotTranscodes()) {
        transcode->inputVideoFrame(frame);
    }
}

void MultiMediaSourceProcessor::resetTracks() {
    MediaSourceDecoder::resetTracks();
    _audio_tracks.clear();
    {
        std::lock_guard<std::mutex> lock(_mtx);
        _transcodes.clear();
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

bool MultiMediaSourceProcessor::isMotionDetectRunning() {
#if defined(ENABLE_MOTION)
    return !!_motion;
#else
    return false;
#endif // ENABLE_MOTION
}

} // namespace mediakit

#endif // ENABLE_FFMPEG
