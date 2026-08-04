#if defined(ENABLE_FFMPEG)

#include "TranscodeProcessor.h"
#include "Extension/Factory.h"
#include "Common/Parser.h"
#include "Util/logger.h"

#include <climits>
#include <cstdlib>
#include <strings.h>

using namespace std;
using namespace toolkit;

namespace toolkit {
    StatisticImp(mediakit::TranscodeProcessor)
}

namespace mediakit {

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

    auto codec_it = args.find("transcode_vcodec");
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

    request.width = get_int("transcode_width", request.width);
    request.height = get_int("transcode_height", request.height);
    if (request.width > 0 && request.height > 0 &&
        (request.width > 1920 || request.height > 1080)) {
        const double scale = std::min(1920.0 / request.width, 1080.0 / request.height);
        request.width = std::max(2, static_cast<int>(request.width * scale) & ~1);
        request.height = std::max(2, static_cast<int>(request.height * scale) & ~1);
    } else {
        request.width = std::min(request.width, request.width > 0 ? 1920 : 0);
        request.height = std::min(request.height, request.height > 0 ? 1080 : 0);
    }
    request.bitrate = get_int("transcode_bitrate", request.bitrate);
    request.fps = get_int("transcode_fps", request.fps);
    request.gop = get_int("transcode_gop", request.gop);
    if (request.fps <= 0) {
        request.fps = defaults.transcode_fps > 0 ? defaults.transcode_fps : 5;
    }
    return true;
}

TranscodeProcessor::TranscodeProcessor(const MediaTuple &tuple, const ProtocolOption &option, Config cfg)
    : _tuple(tuple), _cfg(std::move(cfg)) {
    // Publish under a derived stream_id so the transcoded output never collides
    // with the original source (e.g. "cam1" -> "cam1.transcode").
    _tuple.stream += _cfg.stream_suffix;

    // The dedicated fMP4 muxer carries its own demand flag mapped to fmp4_demand.
    ProtocolOption fmp4_opt = option;
    fmp4_opt.enable_fmp4 = true;
    fmp4_opt.fmp4_demand = _cfg.demand;
    _muxer = std::make_shared<FMP4MediaSourceMuxer>(_tuple, fmp4_opt);

    _encoder = std::make_shared<FFmpegEncoder>(_cfg.codec, _cfg.width, _cfg.height, _cfg.fps, _cfg.bitrate, _cfg.gop);
    // NOTE: the encoder callback is invoked synchronously inside inputVideoFrame(),
    // so it is safe to reference `this` directly here.
    _encoder->setOnEncode([this](const Frame::Ptr &frame) {
        // Route the encoded frame through MediaSink for track-ready management.
        MediaSink::inputFrame(frame);
    });

    if (!_cfg.overlay_components.empty() || !_cfg.overlay_options.privacy_masks.empty()) {
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
    if (_muxer) {
        _muxer->setListener(shared_from_this());
    }
}

void TranscodeProcessor::addAudioTrack(const Track::Ptr &track) {
    if (!track) {
        return;
    }
    MediaSink::addTrack(track);
    _have_audio = true;
}

void TranscodeProcessor::finalizeTracks() {
    setMaxTrackCount(_have_audio ? 2 : 1);
    MediaSink::addTrackCompleted();
}

bool TranscodeProcessor::inputVideoFrame(const FFmpegFrame::Ptr &frame) {
    if (!_encoder || !frame) {
        return false;
    }
    // On-demand: once the source is registered, skip the costly overlay+encode
    // while nobody is watching. Force an IDR when a viewer (re)connects so the
    // client can start decoding immediately.
    if (_primed && _cfg.demand) {
        bool now_enabled = _muxer && _muxer->isEnabled();
        if (now_enabled && !_last_enabled) {
            _encoder->requestKeyFrame();
        }
        _last_enabled = now_enabled;
        if (!now_enabled) {
            return true;
        }
    }

    // Pace frames before the expensive overlay graph. FFmpegEncoder also
    // guards its input rate, but doing the first gate here avoids decoding the
    // overlay image and privacy filters for frames that cannot be encoded.
    if (frame->get()->pts != AV_NOPTS_VALUE) {
        const int output_fps = _cfg.fps > 0 ? _cfg.fps : 5;
        const int64_t frame_interval_ms = std::max<int64_t>(1, 1000 / output_fps);
        if (_last_overlay_input_pts != AV_NOPTS_VALUE) {
            if (frame->get()->pts < _last_overlay_input_pts ||
                frame->get()->pts - _last_overlay_input_pts < frame_interval_ms) {
                return true;
            }
        }
        _last_overlay_input_pts = frame->get()->pts;
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
            InfoL << "TranscodeProcessor: scale before overlay to "
                  << output_width << "x" << output_height;
        }
        in = _pre_overlay_sws->inputFrame(frame);
        if (!in) {
            WarnL << "TranscodeProcessor: pre-overlay scale failed";
            return false;
        }
        in = _overlay->inputFrame(in);
    }
    return _encoder->inputFrame(in);
}

bool TranscodeProcessor::inputAudioFrame(const Frame::Ptr &frame) {
    if (!frame) {
        return false;
    }
    return MediaSink::inputFrame(frame);
}

size_t TranscodeProcessor::totalCount() {
    return toolkit::ObjectStatistic<TranscodeProcessor>::count();
}

bool TranscodeProcessor::onTrackReady(const Track::Ptr &track) {
    return _muxer ? _muxer->addTrack(track) : false;
}

void TranscodeProcessor::onAllTrackReady() {
    if (_muxer) {
        _muxer->addTrackCompleted();
    }
    _primed = true;
    InfoL << "Transcode stream ready: " << _tuple.shortUrl();
}

bool TranscodeProcessor::onTrackFrame(const Frame::Ptr &frame) {
    return _muxer ? _muxer->inputFrame(frame) : false;
}

} // namespace mediakit

#endif // ENABLE_FFMPEG
