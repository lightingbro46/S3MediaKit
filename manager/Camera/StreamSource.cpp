#include "StreamSource.h"
#include "Extension/Plugin.h"
#include "../../server/WebApi.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

bool equalStreamTuple(const StreamTuple &a, const StreamTuple &b) {
    return a.vhost == b.vhost && a.device_id == b.device_id &&
            a.stream_id == b.stream_id && a.full_url == b.full_url;
}

const string getStreamTypeString(int type) {
#define SWITCH_CASE(type) case type : return #type
    switch (type) {
        SWITCH_CASE(PrimaryStream);
        SWITCH_CASE(SecondaryStream);
        default : return "unknown";
    }
}

StreamSource::StreamSource(const StreamTuple &tuple, bool record, int rtp_type, int media_port, float timeout_sec) 
    : _tuple(std::move(tuple)), _record(record), _rtp_type(rtp_type), _media_port(media_port), _timeout_sec(timeout_sec) {

    _full_url = tuple.full_url;
    if (_media_port) {
        _full_url = replacePort(tuple.full_url, _media_port);
    }
}

StreamSource::~StreamSource() {
    closePlayer();
}

void StreamSource::start() {
    createPlayer();
}

void StreamSource::createPlayer() {
    MediaTuple tuple = { .vhost = DEFAULT_VHOST, .app = _tuple.device_id, .stream = _tuple.stream_id, .params = "" };

    ProtocolOption option;
    option.enable_mp4 = _record;

    weak_ptr<StreamSource> weak_self = shared_from_this();
    auto setup_player = [weak_self](const string &err, const PlayerProxy::Ptr &player) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }

        // return if player proxy with same key exist
        if (!err.empty()) {
            WarnL << "Create stream player proxy " << strong_self->_tuple.shortUrl() << " failed: " << err;
            return;
        }

        (*player)[Client::kRtpType] = strong_self->_rtp_type;

        if (strong_self->_timeout_sec > 0.1f) {
            // Play handshake timeout
            (*player)[Client::kTimeoutMS] = strong_self->_timeout_sec * 1000;
        }

        player->setPlayCallbackOnce([weak_self](const SockException &ex) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->_live = !ex ? true : false;
            strong_self->_status = ex.what();
            TraceL << "setPlayCallbackOnce: live=" << strong_self->_live << " status=" << strong_self->_status;

            if (strong_self->_on_ready) {
                strong_self->_on_ready();
            }
        });

        player->setOnConnect([weak_self](const TranslationInfo &info) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            if (!strong_self->_live) {
                strong_self->_live = true;
                strong_self->_status = "play rtsp success";
            }
            strong_self->_info = info;
            TraceL << "setOnConnect: live=" << strong_self->_live << " status=" << strong_self->_status;
            
            if (strong_self->_on_change) {
                strong_self->_on_change();
            }
        });

        player->setOnDisconnect([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            if (strong_self->_live) {
                strong_self->_live = false;
                strong_self->_status = "self-disconnect";
            }
            TraceL << "setOnDisconnect: live=" << strong_self->_live << " status=" << strong_self->_status;

            if (strong_self->_on_change) {
                strong_self->_on_change();
            }
        });

        player->setOnClose([weak_self](const SockException &ex) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->_live = !ex ? true : false;
            strong_self->_status = ex.what();
            TraceL << "setOnClose: live=" << strong_self->_live << " status=" << strong_self->_status;
            
            if (strong_self->_on_change) {
                strong_self->_on_change();
            }
        });

        player->play(strong_self->_tuple.full_url);
        strong_self->_player = player;
        DebugL << "Create stream player proxy: " << strong_self->_tuple.shortUrl();
    };

    addStreamProxy(tuple, option, setup_player);
}

void StreamSource::closePlayer() {
    MediaTuple tuple = {
        .vhost = DEFAULT_VHOST,
        .app = _tuple.device_id,
        .stream = _tuple.stream_id,
        .params = ""
    };
    delStreamProxy(tuple);
    _player.reset();
    DebugL << "Close stream player proxy: " << _tuple.shortUrl();
}

TranslationInfo StreamSource::getTranslationInfo() {
    auto media_src = MediaSource::find(RTSP_SCHEMA, _tuple.vhost, _tuple.device_id, _tuple.stream_id);
    if (media_src) {
        _info.byte_speed = media_src->getBytesSpeed();
        _info.start_time_stamp = media_src->getCreateStamp();
        _info.stream_info.clear();
        auto tracks = media_src->getTracks();
        for (auto &track : tracks) {
            track->update();
            _info.stream_info.emplace_back();
            auto &back = _info.stream_info.back();
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
    
    return _info;
}

} // namespace managerkit
