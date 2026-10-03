#include "MjpegIngestTranscoder.h"

#include <cmath>

#include "Common/config.h"
#include "Extension/Factory.h"
#include "Util/logger.h"
#include "Util/util.h"

#if defined(ENABLE_FFMPEG)
#include "Codec/Transcode.h"
#endif

using namespace std;
using namespace toolkit;

namespace mediakit {

namespace {
std::atomic<size_t> s_active { 0 };
} // namespace

#if defined(ENABLE_FFMPEG)

// Decoder and encoder live behind Impl so callers do not need FFmpeg headers.
class MjpegIngestTranscoder::Impl {
public:
    FFmpegDecoder::Ptr decoder;
    // Only touched on the decoder thread (and by the decoder flush in reset()).
    FFmpegEncoder::Ptr encoder;
    int width = 0;
    int height = 0;
    int fps = 0;
    int gop = 0;
    int bitrate = 0;
    CodecId codec = CodecH264;

    void reset() {
        if (decoder) {
            // Joins the decoder thread, then flushes the decoder into the still alive encoder.
            decoder->stopThread(true);
            decoder = nullptr;
        }
        if (encoder) {
            encoder->flush();
            encoder = nullptr;
        }
    }
};

static CodecId ingestCodec() {
    GET_CONFIG(string, codec, Mjpeg::kIngestCodec);
    return strcasecmp(codec.data(), "H265") == 0 || strcasecmp(codec.data(), "HEVC") == 0 ? CodecH265 : CodecH264;
}

bool MjpegIngestTranscoder::enabled() {
    GET_CONFIG(bool, enable, Mjpeg::kIngestTranscode);
    return enable;
}

MjpegIngestTranscoder::Ptr MjpegIngestTranscoder::create(const Track::Ptr &source, const EventPoller::Ptr &poller) {
    if (!source || source->getCodecId() != CodecJPEG || !poller || !enabled()) {
        return nullptr;
    }
    GET_CONFIG(size_t, max_sessions, Mjpeg::kIngestMaxSessions);
    if (max_sessions && s_active.load() >= max_sessions) {
        WarnL << "MJPEG ingest transcode limit reached (" << max_sessions << "), pass the JPEG track through";
        return nullptr;
    }
    Ptr ret(new MjpegIngestTranscoder(source, poller, ingestCodec()));
    ret->_weak_self = ret;
    return ret;
}

MjpegIngestTranscoder::MjpegIngestTranscoder(const Track::Ptr &source, const EventPoller::Ptr &poller, CodecId codec)
    : _source(source), _poller(poller), _impl(new Impl) {
    _impl->codec = codec;
    _output = Factory::getTrackByCodecId(codec);
    CHECK(_output, "Create ingest output track failed: ", getCodecName(codec));
    _output->setIndex(source->getIndex());
    _output->setOriginCodecId(source->getCodecId());
    ++s_active;
}

MjpegIngestTranscoder::~MjpegIngestTranscoder() {
    _impl->reset();
    --s_active;
    InfoL << "MJPEG ingest transcode closed, frames in: " << _frames_in << ", out: " << _frames_out.load();
}

bool MjpegIngestTranscoder::inputFrame(const Frame::Ptr &frame) {
    if (frame->getTrackType() != TrackVideo) {
        return false;
    }
    ++_frames_in;
    auto &impl = *_impl;
    if (!impl.decoder) {
        // The JPEG track forwards frames only once it is ready, so size and fps are known here.
        auto video = dynamic_pointer_cast<VideoTrack>(_source);
        float fps = video ? video->getVideoFps() : 0;
        impl.fps = fps > 0 ? std::max(1, (int)std::lround(fps)) : 25;
        GET_CONFIG(int, gop_sec, Mjpeg::kIngestGopSec);
        GET_CONFIG(int, bitrate, Mjpeg::kIngestBitrate);
        impl.gop = impl.fps * std::max(1, gop_sec);
        impl.bitrate = bitrate;
        impl.decoder = std::make_shared<FFmpegDecoder>(_source, 0);
        // Runs on the decoder thread, which Impl::reset() joins before this object goes away.
        impl.decoder->setOnDecode([this](const FFmpegFrame::Ptr &decoded) {
            auto &impl = *_impl;
            auto src = decoded->get();
            if (!impl.encoder || src->width != impl.width || src->height != impl.height) {
                if (impl.encoder) {
                    InfoL << "MJPEG ingest resolution changed " << impl.width << "x" << impl.height << " -> " << src->width
                          << "x" << src->height << ", reopen encoder";
                    impl.encoder->flush();
                }
                impl.width = src->width;
                impl.height = src->height;
                impl.encoder = std::make_shared<FFmpegEncoder>(impl.codec, 0, 0, impl.fps, impl.bitrate, impl.gop);
                impl.encoder->setOnEncode([this](const Frame::Ptr &encoded) { onEncoded(encoded); });
                impl.encoder->requestKeyFrame();
            }
            impl.encoder->inputFrame(decoded, src->pts);
        });
        InfoL << "MJPEG ingest transcode to " << getCodecName(impl.codec) << ", " << impl.fps << " fps, gop " << impl.gop;
    }
    // Every JPEG frame is a keyframe; a full decoder queue only lowers the output frame rate.
    return impl.decoder->inputFrame(frame, true, true, false);
}

void MjpegIngestTranscoder::onEncoded(const Frame::Ptr &frame) {
    // The encoder frame points into its AVPacket; copy it before leaving the decoder thread.
    auto cached = Frame::getCacheAbleFrame(frame);
    // The muxer matches frames to tracks by index.
    cached->setIndex(_output->getIndex());
    ++_frames_out;
    // Not shared_from_this(): the destructor flush still emits frames, which are dropped here.
    auto weak_self = _weak_self;
    _poller->async([weak_self, cached]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        self->_output->inputFrame(cached);
    }, false);
}

size_t MjpegIngestTranscoder::activeCount() {
    return s_active.load();
}

#else // ENABLE_FFMPEG

class MjpegIngestTranscoder::Impl {};

bool MjpegIngestTranscoder::enabled() {
    return false;
}

MjpegIngestTranscoder::Ptr MjpegIngestTranscoder::create(const Track::Ptr &, const EventPoller::Ptr &) {
    return nullptr;
}

MjpegIngestTranscoder::MjpegIngestTranscoder(const Track::Ptr &source, const EventPoller::Ptr &poller, CodecId)
    : _source(source), _poller(poller) {}

MjpegIngestTranscoder::~MjpegIngestTranscoder() = default;

bool MjpegIngestTranscoder::inputFrame(const Frame::Ptr &) {
    return false;
}

void MjpegIngestTranscoder::onEncoded(const Frame::Ptr &) {}

size_t MjpegIngestTranscoder::activeCount() {
    return 0;
}

#endif // ENABLE_FFMPEG

} // namespace mediakit
