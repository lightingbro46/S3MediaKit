#include "StreamSource.h"
#include "Extension/Plugin.h"
#include "server/WebApi.h"
#include "server/Manager.h"
#include "Common/StrUtil.h"

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

StreamSource::StreamSource(const StreamTuple &tuple, bool record_mp4, bool record_audio, int rtp_type, int media_port, string username, string password, float timeout_sec) 
    : _tuple(std::move(tuple)), _record_mp4(record_mp4), _record_audio(record_audio), _rtp_type(rtp_type), _media_port(media_port),
     _username(std::move(username)), _password(std::move(password)), _timeout_sec(timeout_sec) {

    _full_url = tuple.full_url;

    if (_full_url.find("@") == string::npos && !_username.empty() && !_password.empty()) {
        // insert auth info to url if not exist
        _full_url = UriUtils::replaceCredentials(_full_url, _username, _password);
    }

    if (_media_port) {
        // replace port in url if media_port is specified
        _full_url = UriUtils::replacePort(_full_url, _media_port);
    }

    if (!_timeout_sec) {
        GET_CONFIG(float, timeoutSec, Manager::kMaxStreamTimeoutSec);
        _timeout_sec = timeoutSec;
    }
}

StreamSource::~StreamSource() {
    closePlayer();
}

void StreamSource::start() {
    createPlayer();
}

void StreamSource::createPlayer() {
    MediaTuple tuple(DEFAULT_VHOST, _tuple.device_id, _tuple.stream_id, "");

    ProtocolOption option;
    option.enable_mp4 = _record_mp4;
    option.enable_rtsp = true;
    option.enable_hls = true;
    option.enable_motion = false;
    option.enable_audio = _record_audio;

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
            
            if (strong_self->_on_update) {
                strong_self->_on_update(strong_self->_live, strong_self->_status, &strong_self->_info);
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

            if (strong_self->_on_update) {
                strong_self->_on_update(strong_self->_live, strong_self->_status, nullptr);
            }
        });

        // Note: onClose is called when the player proxy is closed itself
        player->setOnClose([weak_self](const SockException &ex) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->_live = !ex ? true : false;
            strong_self->_status = ex.what();
            TraceL << "setOnClose: live=" << strong_self->_live << " status=" << strong_self->_status;
            
            if (strong_self->_on_update) {
                strong_self->_on_update(strong_self->_live, strong_self->_status, nullptr);
            }
        });

        player->play(strong_self->_full_url);
        strong_self->_player = player;
        DebugL << "Created stream player proxy: " << strong_self->_tuple.shortUrl();
    };

    addStreamProxy(tuple, option, setup_player);
}

void StreamSource::closePlayer() {
    MediaTuple tuple(DEFAULT_VHOST, _tuple.device_id, _tuple.stream_id, "");
    delStreamProxy(tuple);
    _player.reset();
    DebugL << "Closed stream player proxy: " << _tuple.shortUrl();
    if (_on_update) {
        _on_update(false, "self-closed", nullptr);
    }
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

bool StreamSource::setupRecord(bool start, bool archived, int pre_sec) {
    auto strong_player = _player.lock();
    if (!strong_player) {
        return false;
    }

    strong_player->setupRecord(Recorder::type_mp4, start, archived);
    return true;
}

} // namespace managerkit
