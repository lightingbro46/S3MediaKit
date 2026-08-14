#if defined(ENABLE_FFMPEG)

#include "TranscodeProcessor.h"
#include "Extension/Factory.h"
#include "Common/Parser.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Util/logger.h"

#include <climits>
#include <cstdlib>
#include <algorithm>
#include <strings.h>

using namespace std;
using namespace toolkit;

namespace toolkit {
    StatisticImp(mediakit::TranscodeProcessor)
}

namespace mediakit {

namespace {
const int kMuteAudioIndex = 0xFFFF;
}

bool parseTranscodeRequest(const std::string &params,
                           const ProtocolOption &defaults,
                           TranscodeRequest &request,
                           std::string &error) {
    request = TranscodeRequest();
    request.width = 0;
    request.height = 0;
    request.bitrate = 0;
    request.fps = 5;
    request.gop = 0;
    error.clear();

    const auto args = Parser::parseArgs(params);
    auto enabled_it = args.find("transcode");
    if (enabled_it == args.end() || strcasecmp(enabled_it->second.data(), "true") != 0) {
        return true;
    }

    request.enabled = true;
    request.width = defaults.transcode_width;
    request.height = defaults.transcode_height;
    request.bitrate = defaults.transcode_bitrate;
    request.fps = defaults.transcode_fps > 0 ? defaults.transcode_fps : 5;
    request.gop = defaults.transcode_gop;

    auto codec_it = args.find("target_vcodec");
    if (codec_it != args.end() && !codec_it->second.empty()) {
        if (strcasecmp(codec_it->second.data(), "h264") == 0) {
            request.codec = CodecH264;
        } else if (strcasecmp(codec_it->second.data(), "h265") == 0) {
            request.codec = CodecH265;
        } else {
            error = "transcode_vcodec must be h264 or h265";
            return false;
        }
    }

    auto get_int = [&args](const std::string &name, int fallback) {
        auto it = args.find(name);
        if (it == args.end() || it->second.empty()) {
            return fallback;
        }
        char *end = nullptr;
        const long value = strtol(it->second.data(), &end, 10);
        if (end == it->second.data() || *end != '\0' || value < 0 || value > INT_MAX) {
            return fallback;
        }
        return static_cast<int>(value);
    };

    request.width = get_int("target_width", request.width);
    request.height = get_int("target_height", request.height);
    if (request.width > 0 && request.height > 0 &&
        (request.width > 1920 || request.height > 1080)) {
        const double scale = std::min(1920.0 / request.width, 1080.0 / request.height);
        request.width = std::max(2, static_cast<int>(request.width * scale) & ~1);
        request.height = std::max(2, static_cast<int>(request.height * scale) & ~1);
    } else {
        request.width = std::min(request.width, request.width > 0 ? 1920 : 0);
        request.height = std::min(request.height, request.height > 0 ? 1080 : 0);
    }
    request.bitrate = get_int("target_bitrate", request.bitrate);
    request.fps = get_int("target_fps", request.fps);
    request.gop = get_int("target_gop", request.gop);
    if (request.fps <= 0) {
        request.fps = defaults.transcode_fps > 0 ? defaults.transcode_fps : 5;
    } else {
        // Limit the maximum FPS to 20 to avoid excessive CPU usage.
        request.fps = std::min(request.fps, 25);
    }
    // Limit the GOP to a reasonable range based on the FPS.
    if (request.gop <= 0) {
        request.gop = std::max(5, std::min(request.fps * 2, request.fps * 4));
    } else {
        request.gop = std::max(request.gop, request.fps * 4);
    }
    return true;
}

TranscodeProcessor::TranscodeProcessor(const MediaTuple &tuple, const ProtocolOption &option, Config cfg,
                                       const toolkit::EventPoller::Ptr &poller)
    : _tuple(tuple), _option(option), _cfg(std::move(cfg)),
      _poller(poller ? poller : EventPollerPool::Instance().getPoller(false)) {
    // Publish under a derived stream_id so the transcoded output never collides
    // with the original source (e.g. "cam1" -> "cam1.transcode").
    _tuple.stream += _cfg.stream_suffix;
    _frame_rate_filter = std::make_shared<FFmpegFrameRateFilter>(_cfg.fps, _cfg.source_fps);

    // Create the muxer before the first encoded frame. Tracks are attached
    // synchronously and completed on the muxer's owner poller.
    createMuxer();

    _encoder = std::make_shared<FFmpegEncoder>(_cfg.codec, _cfg.width, _cfg.height, _cfg.fps, _cfg.bitrate, _cfg.gop);
    // NOTE: the encoder callback is invoked synchronously inside inputVideoFrame(),
    // so it is safe to reference `this` directly here.
    _encoder->setOnEncode([this](const Frame::Ptr &frame) {
        // Route the encoded frame through MediaSink for track-ready management.
        MediaSink::inputFrame(frame);
    });

    if (!_cfg.overlay_components.empty() || !_cfg.overlay_options.privacy_masks.empty() ||
        !_cfg.overlay_options.prebuilt_svg_path.empty()) {
        _overlay = std::make_shared<TranscodeOverlay>(_cfg.overlay_components, _cfg.overlay_options);
    } else if (!_cfg.overlay_image.empty()) {
        _overlay = std::make_shared<TranscodeOverlay>(_cfg.overlay_image, _cfg.overlay_x, _cfg.overlay_y);
    }

    // Do not synthesize a silent audio track: keep audio pass-through only.
    enableMuteAudio(false);

    // Register the (initially unready) video output track. It becomes ready once
    // the encoder emits its first keyframe carrying SPS/PPS.
    auto video_track = Factory::getTrackByCodecId(_cfg.codec);
    if (video_track) {
        MediaSink::addTrack(video_track);
    } else {
        WarnL << "TranscodeProcessor: cannot create output track for codec " << _cfg.codec;
    }
}

void TranscodeProcessor::createMuxer() {
    // Reuse the existing multi-protocol muxer. All configured output branches
    // share this processor; no additional decode/overlay/encode pipeline is
    // created for another protocol.
    ProtocolOption output_option = _option;
    output_option.enable_mp4 = false;
    output_option.enable_gop_cache = false;
    output_option.gop_cache_size = 0;
    output_option.mp4_as_player = false;
    output_option.add_mute_audio = false;
    output_option.modify_stamp = ProtocolOption::kModifyStampOff;
    output_option.paced_sender_ms = 10;
    // Forward the per-schema demand policy from the original MediaSource.
    // Keep rtsp_demand, rtmp_demand, hls_demand, ts_demand and fmp4_demand
    // from _option so each output schema follows the source configuration.
    // The derived transcode stream has its own lifecycle policy: when it was
    // created on demand, MediaSourceEvent must auto-close it after the last
    // reader disappears and the configured no-reader delay expires.
    output_option.auto_close = _cfg.demand;
    output_option.enable_audio = true;
    output_option.enable_transcode = false;
    output_option.enable_motion = false;
    auto owner_poller = EventPollerPool::Instance().getPoller(false);
    if (owner_poller && !owner_poller->isCurrentThread()) {
        owner_poller->sync([this, output_option]() {
            _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, 0.0f, output_option);
        });
    } else {
        _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, 0.0f, output_option);
    }
}

void TranscodeProcessor::addTrackToMuxer(const Track::Ptr &track) {
    if (!_muxer || !track) {
        return;
    }

    auto owner_poller = _muxer->getOwnerPoller(MediaSource::NullMediaSource());
    auto muxer = _muxer;
    if (owner_poller && !owner_poller->isCurrentThread()) {
        owner_poller->sync([muxer, track]() {
            muxer->addTrack(track);
        });
    } else {
        muxer->addTrack(track);
    }
}

void TranscodeProcessor::completeMuxerTracks() {
    if (!_muxer) {
        return;
    }
    auto owner_poller = _muxer->getOwnerPoller(MediaSource::NullMediaSource());
    if (owner_poller && !owner_poller->isCurrentThread()) {
        auto muxer = _muxer;
        owner_poller->sync([muxer]() {
            muxer->addTrackCompleted();
        });
    } else {
        _muxer->addTrackCompleted();
    }
}

TranscodeProcessor::~TranscodeProcessor() {
    try {
        if (_encoder) {
            _encoder->flush();
        }
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void TranscodeProcessor::setListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);
    // The muxer is created in the constructor, but it can only wire its
    // listener after shared_from_this() is valid.
    if (_muxer) {
        auto muxer = _muxer;
        auto self = shared_from_this();
        auto owner_poller = _muxer->getOwnerPoller(MediaSource::NullMediaSource());
        owner_poller->sync([muxer, self]() {
            muxer->setMediaListener(self);
        });
    }
}

void TranscodeProcessor::setOnClosed(const std::function<void(const Ptr &)> &callback) {
    _on_closed = callback;
}

void TranscodeProcessor::addAudioTrack(const Track::Ptr &track) {
    if (!track || track->getTrackType() != TrackAudio) {
        return;
    }
    if (_have_audio) {
        if (_audio_track_index != track->getIndex()) {
            WarnL << "Ignore additional transcode audio track index=" << track->getIndex()
                  << ", selected index=" << _audio_track_index;
        }
        return;
    }
    if (isAllTrackReady()) {
        WarnL << "Ignore late source audio track index=" << track->getIndex()
              << ": transcode tracks are already finalized";
        return;
    }
    // Source audio (including the source-generated mute track) is the only
    // audio producer for this transcode output.
    enableMuteAudio(false);
    // Never attach this MediaSink as another delegate of the source Track.
    // The cloned track is fed exclusively by inputAudioFrame() through the
    // serialized transcode ring, preventing direct-source plus ring delivery.
    auto output_track = track->clone();
    if (!output_track) {
        WarnL << "Failed to clone transcode audio track index=" << track->getIndex();
        return;
    }
    MediaSink::addTrack(output_track);
    _have_audio = true;
    _audio_track_index = track->getIndex();
}

void TranscodeProcessor::finalizeTracks() {
    DebugL << "Finalize transcode source audio: "
           << (_have_audio ? (_audio_track_index == kMuteAudioIndex ? "mute" : "real") : "none")
           << ", index=" << _audio_track_index;
    setMaxTrackCount(_have_audio ? 2 : 1);
    MediaSink::addTrackCompleted();
}

bool TranscodeProcessor::inputVideoFrame(const FFmpegFrame::Ptr &frame) {
    if (!_encoder || !frame) {
        return false;
    }
    const int64_t input_pts = frame->get()->pts;
    if (input_pts != AV_NOPTS_VALUE) {
        if (_source_pts_base == AV_NOPTS_VALUE) {
            _source_pts_base = input_pts;
        }
    }
    // No GOP replay is used for a newly attached reader. Request an IDR before
    // feeding its first frame so the derived stream can publish a decodable
    // sequence immediately.
    if (_first_video) {
        _encoder->requestKeyFrame();
        _first_video = false;
    }
    // Gate demand output through the muxer and force an IDR when the first
    // reader becomes active (or reconnects). This makes the first delivered
    // frame independently decodable.
    if (_primed && _cfg.demand) {
        const bool now_enabled = _muxer && _muxer->isEnabled();
        if (now_enabled && !_last_enabled) {
            // Do not replay a frame retained by the CFR look-ahead from the
            // previous reader session.
            _frame_rate_filter->reset();
            _encoder->requestKeyFrame();
        }
        _last_enabled = now_enabled;
        if (!now_enabled) {
            return true;
        }
    }

    return _frame_rate_filter->inputFrame(
        frame, [this](const FFmpegFrame::Ptr &filtered) {
            return processFilteredVideoFrame(filtered);
        });
}

void TranscodeProcessor::setSourceFps(double source_fps) {
    _cfg.source_fps = source_fps;
    if (_frame_rate_filter) {
        _frame_rate_filter->setSourceFps(source_fps);
    }
}

bool TranscodeProcessor::processFilteredVideoFrame(const FFmpegFrame::Ptr &frame) {
    if (!frame || !frame->get()) {
        return false;
    }
    FFmpegFrame::Ptr in = frame;
    if (_overlay && _overlay->valid()) {
        int output_width = 0;
        int output_height = 0;
        getTranscodeOutputSize(frame->get()->width, frame->get()->height,
                               _cfg.width, _cfg.height, output_width, output_height);
        if (!_pre_overlay_sws || _pre_overlay_width != output_width ||
            _pre_overlay_height != output_height) {
            _pre_overlay_sws = std::make_shared<FFmpegSws>(AV_PIX_FMT_YUV420P,
                                                            output_width, output_height);
            _pre_overlay_width = output_width;
            _pre_overlay_height = output_height;
            DebugL << "TranscodeProcessor: scale before overlay to " << output_width << "x" << output_height;
        }
        in = _pre_overlay_sws->inputFrame(frame);
        if (!in) {
            WarnL << "TranscodeProcessor: pre-overlay scale failed";
            return false;
        }
        in = _overlay->inputFrame(in);
    }
    return _encoder->inputFrame(in, frame->get()->pts);
}

bool TranscodeProcessor::inputAudioFrame(const Frame::Ptr &frame) {
    if (!frame || !_have_audio || frame->getIndex() != _audio_track_index ||
        _source_pts_base == AV_NOPTS_VALUE) {
        return false;
    }
    // Keep pass-through audio on the same relative timeline as the first
    // decoded video frame. FrameStamp only wraps the buffer; it does not copy
    // the audio payload.
    auto stamped = std::make_shared<FrameStamp>(frame);
    const int64_t dts = static_cast<int64_t>(frame->dts()) - _source_pts_base;
    const int64_t source_pts = frame->pts() ? static_cast<int64_t>(frame->pts())
                                            : static_cast<int64_t>(frame->dts());
    const int64_t pts = source_pts - _source_pts_base;
    stamped->setStamp(std::max<int64_t>(0, dts), std::max<int64_t>(0, pts));
    return MediaSink::inputFrame(stamped);
}

bool TranscodeProcessor::isEnabled() {
    return _muxer && _muxer->isEnabled();
}

bool TranscodeProcessor::onTrackReady(const Track::Ptr &track) {
    if (track) {
        addTrackToMuxer(track);
    }
    return true;
}

void TranscodeProcessor::onAllTrackReady() {
    completeMuxerTracks();
    _primed = true;
    InfoL << "Transcode stream ready: " << _tuple.shortUrl();
}

bool TranscodeProcessor::onTrackFrame(const Frame::Ptr &frame) {
    return _muxer && frame ? _muxer->inputFrame(frame) : false;
}

void TranscodeProcessor::onReaderChanged(MediaSource &sender, int size) {
    TraceL << "Transcode reader state: " << _tuple.shortUrl() << ", readers=" << size;
    // Keep the derived output timer local, then notify the owning processor so
    // it can aggregate readers across all output schemas.
    MediaSourceEvent::onReaderChanged(sender, size);
    auto listener = getDelegate();
    if (listener) {
        listener->onReaderChanged(sender, size);
    }
}

bool TranscodeProcessor::close(MediaSource &sender) {
    // Closing a derived output must not propagate to the shared source
    // processor. MultiMediaSourceMuxer has already received this close event
    // and will release its protocol branches; notify only the owner of this
    // TranscodeProcessor after that cleanup has completed.
    const bool ret = true;
    auto callback = std::move(_on_closed);
    _on_closed = nullptr;
    if (!callback) {
        return ret;
    }

    // MultiMediaSourceMuxer clears all protocol branches after forwarding the
    // close event. Defer the callback so totalReaderCount() observes the
    // completed close, not the intermediate protocol state.
    auto self = shared_from_this();
    auto muxer = _muxer;
    auto check_closed = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak_check_closed = check_closed;
    *check_closed = [callback, self, muxer, weak_check_closed]() {
        if (muxer && muxer->totalReaderCount()) {
            WarnL << "Transcode close callback postponed: readers remain, stream=" << self->_tuple.shortUrl();
            auto check = weak_check_closed.lock();
            if (check) {
                self->_poller->async(*check, false);
            }
            return;
        }
        callback(self);
    };
    _poller->async(*check_closed, false);
    return ret;
}

int TranscodeProcessor::totalReaderCount() const {
    return _muxer ? _muxer->totalReaderCount() : 0;
}

MediaOriginType TranscodeProcessor::getOriginType(MediaSource &sender) const {
    // A transcode output is a derived source, not a new camera pull. Do not
    // forward the origin type from the shared source processor.
    return MediaOriginType::transcode;
}

std::string TranscodeProcessor::getOriginUrl(MediaSource &sender) const {
    // Report the derived stream URL instead of the original camera URL.
    return _tuple.shortUrl();
}

toolkit::EventPoller::Ptr TranscodeProcessor::getOwnerPoller(MediaSource &sender) {
    return _poller;
}

} // namespace mediakit

#endif // ENABLE_FFMPEG
