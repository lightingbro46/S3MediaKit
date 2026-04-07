#include "StreamSource.h"
#include "Extension/Track.h"
#include "server/WebApi.h"
#include "server/Manager.h"
#include "Common/StrUtil.h"
#include "StreamSink.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

const string getStreamTypeString(int type) {
#define SWITCH_CASE(type) case type : return #type
    switch (type) {
        SWITCH_CASE(PrimaryStream);
        SWITCH_CASE(SecondaryStream);
        default : return "unknown";
    }
}

bool isValidStreamType(int type) {
    return type >= StreamType::PrimaryStream && type < StreamType::StreamMax;
}

StreamSource::StreamSource(int type, const StreamOption &option)
    : _type(type), _option(option) {

    _full_url = _option.tuple.full_url;

    if (_full_url.find("@") == string::npos && !_option.username.empty() && !_option.password.empty()) {
        // insert auth info to url if not exist
        _full_url = UriUtils::replaceCredentials(_full_url, _option.username, _option.password);
    }

    if (_option.media_port > 0) {
        // replace port in url if media_port is specified
        _full_url = UriUtils::replacePort(_full_url, _option.media_port);
    }

    if (!_option.timeout_sec) {
        GET_CONFIG(float, timeoutSec, Manager::kMaxStreamTimeoutSec);
        _option.timeout_sec = timeoutSec;
    }
}

StreamSource::~StreamSource() {
    closePlayer();
}

void StreamSource::setState(bool live, std::string status) {
    _live.store(live, std::memory_order_release);
    auto status_ptr = std::make_shared<const std::string>(std::move(status));
    std::atomic_store_explicit(&_status, status_ptr, std::memory_order_release);
}

void StreamSource::setListener(std::shared_ptr<DeviceSourceEvent> listener) {
    setDelegate(listener);
}

void StreamSource::start() {
    createPlayer();
}

void StreamSource::createPlayer() {
    MediaTuple tuple(DEFAULT_VHOST, _option.tuple.device_id, _option.tuple.stream_id, "quality=" + (_type == StreamType::PrimaryStream) ? "hi" : "lo");

    weak_ptr<StreamSource> weak_self = shared_from_this();
    auto setup_player = [weak_self](const string &err, const PlayerProxy::Ptr &player) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }

        // return if player proxy with same key exist
        if (!err.empty()) {
            WarnL << "Create stream player proxy " << strong_self->_option.tuple.shortUrl() << " failed: " << err;
            return;
        }

        (*player)[Client::kRtpType] = strong_self->_option.rtp_type;

        if (strong_self->_option.timeout_sec > 0.1f) {
            // Play handshake timeout
            (*player)[Client::kTimeoutMS] = strong_self->_option.timeout_sec * 1000;
        }

        player->setPlayCallbackOnce([weak_self](const SockException &ex) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }

            auto live = !ex;
            auto status = ex ? ex.what() : "play callback success";
            strong_self->setState(live, status);
            TraceL << "setPlayCallbackOnce: live=" << live << " status=" << status;
        });

        player->setOnConnect([weak_self](const TranslationInfo &info) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }

            const auto live = true;
            const std::string status = "play rtsp success";
            strong_self->setState(live, status);
            TraceL << "setOnConnect: live=" << live << " status=" << status;
            
            strong_self->onStreamReady(live, status, &info);
        });

        player->setOnDisconnect([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }

            const auto live = false;
            const std::string status = "self-disconnect";
            strong_self->setState(live, status);
            TraceL << "setOnDisconnect: live=" << live << " status=" << status;

            strong_self->onStreamReady(live, status, nullptr);
        });

        // Note: onClose is called when the player proxy is closed itself
        player->setOnClose([weak_self](const SockException &ex) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }

            auto live = !ex;
            auto status = ex ? ex.what() : "closed";
            strong_self->setState(live, status);
            TraceL << "setOnClose: live=" << live << " status=" << status;
            strong_self->onStreamReady(live, status, nullptr);
        });

        player->play(strong_self->_full_url);
        strong_self->_player = player;
        DebugL << "Created stream player proxy: " << strong_self->_option.tuple.shortUrl();
    };

    addStreamProxy(tuple, _option.protocol, setup_player);
}

void StreamSource::closePlayer() {
    MediaTuple tuple(DEFAULT_VHOST, _option.tuple.device_id, _option.tuple.stream_id, "");
    delStreamProxy(tuple);
    _player.reset();
    setState(false, "self-closed");
    DebugL << "Closed stream player proxy: " << _option.tuple.shortUrl();
    onStreamReady(false, "self-closed", nullptr);
}

TranslationInfo StreamSource::getTranslationInfo() {
    TranslationInfo info;
    auto media_src = MediaSource::find(RTSP_SCHEMA, _option.tuple.vhost, _option.tuple.device_id, _option.tuple.stream_id);
    if (media_src) {
        info.byte_speed = media_src->getBytesSpeed();
        info.start_time_stamp = media_src->getCreateStamp();
        info.stream_info.clear();
        auto tracks = media_src->getTracks();
        for (auto &track : tracks) {
            track->update();
            info.stream_info.emplace_back();
            auto &back = info.stream_info.back();
            back.bitrate = track->getBitRate();
            back.codec_type = track->getTrackType();
            back.codec_name = track->getCodecName();
            switch (back.codec_type) {
                case TrackAudio : {
                    auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
                    back.audio_sample_rate = audio_track->getAudioSampleRate();
                    back.audio_channel = audio_track->getAudioChannel();
                    back.audio_sample_bit = audio_track->getAudioSampleBit();
                    break;
                }
                case TrackVideo : {
                    auto video_track = dynamic_pointer_cast<VideoTrack>(track);
                    back.video_width = video_track->getVideoWidth();
                    back.video_height = video_track->getVideoHeight();
                    back.video_fps = video_track->getVideoFps();
                    break;
                }
                default:
                    break;
            }
        }
    }
    
    return info;
}

bool StreamSource::setupRecord(int type, bool start, bool replay_gop) {
    if (!_option.record_mp4) {
        WarnL << "MP4 recording is disabled, cannot setup record for stream: " << _option.tuple.shortUrl();
        return false;
    }

    auto media_src = MediaSource::find(RTSP_SCHEMA, _option.tuple.vhost, _option.tuple.device_id, _option.tuple.stream_id);
    if (!media_src) {
       WarnL << "MediaSource not found for stream: " << _option.tuple.shortUrl();
       return false;
    }

    auto muxer = media_src->getMuxer();
    if (!muxer) {
        WarnL << "MediaSourceMuxer not found for stream: " << _option.tuple.shortUrl();
        return false;
    }

    auto poller = muxer->getOwnerPoller(*media_src);
    if (!poller) {
        WarnL << "EventPoller not found for stream: " << _option.tuple.shortUrl();
        return false;
    }
    poller->async([muxer, type, start, replay_gop, media_src]() {
        auto option = muxer->getOption();
        auto is_recording = muxer->isRecording(static_cast<mediakit::Recorder::type>(type));
        if (start && is_recording) {
            WarnL << "Recording is already enabled, resetting record settings for stream: " << media_src->getMediaTuple().shortUrl();
            muxer->setupRecord(static_cast<mediakit::Recorder::type>(type), false, "", 0);
        }
        muxer->setupRecord(static_cast<mediakit::Recorder::type>(type), start, option.mp4_save_path, option.mp4_max_second, replay_gop);
    });
    return true;
}

mediakit::EventRecordSession::Ptr StreamSource::startEventRecord() {
    if (!_option.record_mp4) {
        WarnL << "MP4 recording is disabled, cannot start event record: " << _option.tuple.shortUrl();
        return nullptr;
    }

    auto media_src = MediaSource::find(RTSP_SCHEMA, _option.tuple.vhost, _option.tuple.device_id, _option.tuple.stream_id);
    if (!media_src) {
        WarnL << "MediaSource not found for event record: " << _option.tuple.shortUrl();
        return nullptr;
    }

    auto muxer = media_src->getMuxer();
    if (!muxer) {
        WarnL << "MediaSourceMuxer not found for event record: " << _option.tuple.shortUrl();
        return nullptr;
    }

    if (!muxer->isRingEnabled()) {
        WarnL << "GOP cache not available for event record: " << _option.tuple.shortUrl();
        return nullptr;
    }

    auto type = Recorder::type_mp4;
    uint32_t back_ms = _option.protocol.pre_record_ms;
    // forward_ms = 0 → infinite clip; terminated by stopRecord() when event ends.
    auto session = muxer->startEventRecord(type, back_ms, 0 /*infinite*/);
    _event_session = session;
    InfoL << "Event record started: stream=" << _option.tuple.shortUrl() << ", back_ms=" << back_ms << " ms, forward_ms=infinite";
    return session;
}

void StreamSource::extendEventRecord() {
    auto session = _event_session;
    if (!session || !session->isActive()) return;
    // Reset deadline to infinite so the clip stays open for the full duration
    // of the new event, however long it lasts.  stop() will re-arm the finite
    // post-tail deadline when the event eventually ends.
    session->resume();
    TraceL << "Event record resumed (deadline reset to infinite): stream=" << _option.tuple.shortUrl();
}

void StreamSource::stopEventRecord(uint32_t extra_overlap_ms) {
    // Do NOT null out _event_session here. Keep the pointer alive so that
    // hasActiveEventSession() returns true during the post_ms tail window.
    // If a new motion event arrives before the tail expires, extendRecord()
    // will be called instead of startRecord(), which would otherwise create
    // a second overlapping primary clip.  _event_session is overwritten in
    // the next startRecord() call, or becomes stale (isActive()==false) and
    // ignored by hasActiveEventSession().
    auto session = _event_session;
    if (!session || !session->isActive()) return;
    uint32_t post_ms = _option.protocol.post_record_ms + extra_overlap_ms;
    session->stop(post_ms);
    InfoL << "Event record stop requested (post_ms=" << post_ms << ", overlap=" << extra_overlap_ms << "): " << _option.tuple.shortUrl();
}

void StreamSource::cancelEventRecord() {
    if (_event_session) {
        if (_event_session->isActive()) {
            // stop(0) sets end_dts = last_written_dts, so the ring-reader lambda
            // will detect DTS exceeded on the very next frame and close the file.
            _event_session->stop(0);
        }
        _event_session = nullptr; // drop our reference; ring reader holds its own
    }
}

void StreamSource::onStreamReady(bool ready, const std::string &status, const TranslationInfo *info) {
    auto data = toolkit::Any(info ? std::make_shared<TranslationInfo>(*info) : nullptr);
    DeviceSourceEventInterceptor::onStreamReady(DeviceSource::NullDeviceSource(), _type, ready, status, data);
}

uint32_t StreamSource::getVideoGopIntervalMs() const {
    auto media_src = MediaSource::find(RTSP_SCHEMA, _option.tuple.vhost, _option.tuple.device_id, _option.tuple.stream_id);
    if (!media_src) return 0;
    for (auto &track : media_src->getTracks(true)) {
        auto video = std::dynamic_pointer_cast<VideoTrack>(track);
        if (video) {
            return static_cast<uint32_t>(video->getVideoGopInterval());
        }
    }
    return 0;
}

} // namespace managerkit
