#include "PlayerProxy.h"
#include "Common/config.h"
#include "Rtmp/RtmpMediaSource.h"
#include "Rtmp/RtmpPlayer.h"
#include "Rtsp/RtspMediaSource.h"
#include "Rtsp/RtspPlayer.h"
#include "Util/MD5.h"
#include "Util/logger.h"
#include "Util/mini.h"

using namespace toolkit;
using namespace std;

namespace mediakit {

PlayerProxy::PlayerProxy(
    const MediaTuple &tuple, const ProtocolOption &option, int retry_count,
    const EventPoller::Ptr &poller, int reconnect_delay_min, int reconnect_delay_max, int reconnect_delay_step)
    : MediaPlayer(poller), _tuple(tuple), _option(option) {
    _retry_count = retry_count;

    setOnClose(nullptr);
    setOnConnect(nullptr);
    setOnDisconnect(nullptr);
    
    _reconnect_delay_min = reconnect_delay_min > 0 ? reconnect_delay_min : 2;
    _reconnect_delay_max = reconnect_delay_max > 0 ? reconnect_delay_max : 60;
    _reconnect_delay_step = reconnect_delay_step > 0 ? reconnect_delay_step : 3;
    _live_secs = 0;
    _live_status = 1;
    _repull_count = 0;
    (*this)[Client::kWaitTrackReady] = false;
}

void PlayerProxy::setPlayCallbackOnce(function<void(const SockException &ex)> cb) {
    _on_play = std::move(cb);
}

void PlayerProxy::setOnClose(function<void(const SockException &ex)> cb) {
    _on_close = cb ? std::move(cb) : [](const SockException &) {};
}

void PlayerProxy::setOnDisconnect(std::function<void(const SockException &ex)> cb) {
    _on_disconnect = cb ? std::move(cb) : [] (const SockException &) {};
}

void PlayerProxy::setOnConnect(std::function<void(const TranslationInfo&)> cb) {
    _on_connect = cb ? std::move(cb) : [](const TranslationInfo&) {};
}

void PlayerProxy::setOnReplayClose(std::function<void(const std::string &proxyKey, const float &progress)> cb) {
    _on_replay_close = std::move(cb);
}

void PlayerProxy::setOnReplayRetry(std::function<bool(const std::string &proxyKey, const float &progress, std::string &newUrl)> cb) {
    _on_replay_retry = std::move(cb);
}

void PlayerProxy::setTranslationInfo()
{
    _transtalion_info.byte_speed = _media_src ? _media_src->getBytesSpeed() : -1;
    _transtalion_info.start_time_stamp = _media_src ? _media_src->getCreateStamp() : 0;
    _transtalion_info.stream_info.clear();
    auto tracks = _muxer->getTracks();
    for (auto &track : tracks) {
        track->update();
        _transtalion_info.stream_info.emplace_back();
        auto &back = _transtalion_info.stream_info.back();
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

static int getMaxTrackSize(const std::string &url) {
    if (url.find(".m3u8") != std::string::npos || url.find(".ts") != std::string::npos) {
        // Only hls and ts protocols support multiple tracks
        return 16;
    }
    return 2;
}

void PlayerProxy::play(const string &strUrlTmp) {
    _option.max_track = getMaxTrackSize(strUrlTmp);
    weak_ptr<PlayerProxy> weakSelf = shared_from_this();
    std::shared_ptr<int> piFailedCnt(new int(0)); // Number of consecutive playback failures
    const bool is_replay = start_with(_tuple.app, kReplayPrefix);
    setOnPlayResult([weakSelf, strUrlTmp, piFailedCnt, is_replay](const SockException &err) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }

        if (strongSelf->_on_play) {
            strongSelf->_on_play(err);
            strongSelf->_on_play = nullptr;
        }

        if (!err) {
            // Cancel the timer to avoid the situation where the hls stream index file fails to reconnect due to network fluctuations and then retries in a loop after successful reconnection
            strongSelf->_timer.reset();
            strongSelf->_live_ticker.resetTime();
            strongSelf->_live_status = 0;
            // Play successfully
            *piFailedCnt = 0; // The number of consecutive playback failed 0
            strongSelf->onPlaySuccess();
            strongSelf->setTranslationInfo();
            strongSelf->_on_connect(strongSelf->_transtalion_info);  

            InfoL << "play " << strUrlTmp << " success";
        } else if (*piFailedCnt < strongSelf->_retry_count || strongSelf->_retry_count < 0) {
            // Play failed, retry playing with delay
            std::string newUrl = strUrlTmp;
            if (is_replay && strongSelf->_on_replay_retry) {
                if (!strongSelf->_on_replay_retry(strongSelf->_tuple.shortUrl(), strongSelf->MediaPlayer::getProgress(), newUrl)) {
                    return;
                }
            }
            strongSelf->_on_disconnect(err);
            strongSelf->rePlay(newUrl, (*piFailedCnt)++);
        } else {
            // Reached the maximum number of retries, callback to close
            if (is_replay && strongSelf->_on_replay_close) {
                strongSelf->_on_replay_close(strongSelf->_tuple.shortUrl(), strongSelf->MediaPlayer::getProgress());
            }
            strongSelf->_on_close(err);
        }
    });
    setOnShutdown([weakSelf, strUrlTmp, piFailedCnt, is_replay](const SockException &err) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }

        // Unregister the stream generated by the direct stream proxy: #532
        strongSelf->setMediaSource(nullptr);

        if (strongSelf->_muxer) {
            auto tracks = strongSelf->MediaPlayer::getTracks(false);
            for (auto &track : tracks) {
                track->delDelegate(strongSelf->_muxer.get());
            }

            GET_CONFIG(bool, reset_when_replay, General::kResetWhenRePlay);
            if (reset_when_replay) {
                strongSelf->_muxer.reset();
            } else {
                strongSelf->_muxer->resetTracks();
            }
        }

        if (*piFailedCnt == 0) {
            // Update the duration for the first time
            strongSelf->_live_secs += strongSelf->_live_ticker.elapsedTime() / 1000;
            strongSelf->_live_ticker.resetTime();
            TraceL << " live secs " << strongSelf->_live_secs;
        }

        // Play interrupted abnormally, retry playing with delay        
        if (*piFailedCnt < strongSelf->_retry_count || strongSelf->_retry_count < 0) {
            std::string newUrl = strUrlTmp;
            if (is_replay && strongSelf->_on_replay_retry) {
                if (!strongSelf->_on_replay_retry(strongSelf->_tuple.shortUrl(), strongSelf->MediaPlayer::getProgress(), newUrl)) {
                    return;
                }
            }
            strongSelf->_repull_count++;
            strongSelf->rePlay(newUrl, (*piFailedCnt)++);
        } else {
            // Reached the maximum number of retries, callback to close
            if (is_replay && strongSelf->_on_replay_close) {
                strongSelf->_on_replay_close(strongSelf->_tuple.shortUrl(), strongSelf->MediaPlayer::getProgress());
            }
            strongSelf->_on_close(err);
        }
    });
    try {
        MediaPlayer::play(strUrlTmp);
    } catch (std::exception &ex) {
        ErrorL << ex.what();
        onPlayResult(SockException(Err_other, ex.what()));
        return;
    }
    _pull_url = strUrlTmp;
    setDirectProxy();
}

void PlayerProxy::setDirectProxy() {
    MediaSource::Ptr mediaSource;
    if (dynamic_pointer_cast<RtspPlayer>(_delegate)) {
        // Rtsp stream
        GET_CONFIG(bool, directProxy, Rtsp::kDirectProxy);
        if (directProxy && _option.enable_rtsp) {
            mediaSource = std::make_shared<RtspMediaSource>(_tuple);
        }
    } else if (dynamic_pointer_cast<RtmpPlayer>(_delegate)) {
        // Rtmp stream
        GET_CONFIG(bool, directProxy, Rtmp::kDirectProxy);
        if (directProxy && _option.enable_rtmp) {
            mediaSource = std::make_shared<RtmpMediaSource>(_tuple);
        }
    }
    if (mediaSource) {
        setMediaSource(mediaSource);
    }
}

PlayerProxy::~PlayerProxy() {
    _timer.reset();
    // Avoid forgetting to callback api request when destructing
    if (_on_play) {
        try {
            _on_play(SockException(Err_shutdown, "player proxy close"));
        } catch (std::exception &ex) {
            WarnL << "Exception occurred: " << ex.what();
        }
        _on_play = nullptr;
    }
}

void PlayerProxy::rePlay(const string &strUrl, int iFailedCnt) {
    auto iDelay = MAX(_reconnect_delay_min * 1000, MIN(iFailedCnt * _reconnect_delay_step * 1000, _reconnect_delay_max * 1000));
    weak_ptr<PlayerProxy> weakSelf = shared_from_this();
    _timer = std::make_shared<Timer>(
        iDelay / 1000.0f,
        [weakSelf, strUrl, iFailedCnt]() {
            // The more times the playback fails, the longer the delay
            auto strongPlayer = weakSelf.lock();
            if (!strongPlayer) {
                return false;
            }
            WarnL << "Try playback again[" << iFailedCnt << "]:" << strUrl;
            strongPlayer->MediaPlayer::play(strUrl);
            strongPlayer->setDirectProxy();
            return false;
        },
        getPoller());
}

bool PlayerProxy::close(MediaSource &sender) {
    // Notify it to stop pushing the stream
    _muxer = nullptr;
    setMediaSource(nullptr);
    teardown();
    _on_close(SockException(Err_shutdown, "closed by user"));
    WarnL << "close media: " << sender.getUrl();
    return true;
}

int PlayerProxy::totalReaderCount() {
    return (_muxer ? _muxer->totalReaderCount() : 0) + (_media_src ? _media_src->readerCount() : 0);
}

int PlayerProxy::totalReaderCount(MediaSource &sender) {
    return totalReaderCount();
}

MediaOriginType PlayerProxy::getOriginType(MediaSource &sender) const {
    return MediaOriginType::pull;
}

string PlayerProxy::getOriginUrl(MediaSource &sender) const {
    return _pull_url;
}

std::shared_ptr<SockInfo> PlayerProxy::getOriginSock(MediaSource &sender) const {
    return getSockInfo();
}

float PlayerProxy::getLossRate(MediaSource &sender, TrackType type) {
    return getPacketLossRate(type);
}

toolkit::EventPoller::Ptr PlayerProxy::getOwnerPoller(MediaSource &sender) { 
    return getPoller();
}

TranslationInfo PlayerProxy::getTranslationInfo() {
    return _transtalion_info;
}

void PlayerProxy::onPlaySuccess() {
    GET_CONFIG(bool, reset_when_replay, General::kResetWhenRePlay);
    if (dynamic_pointer_cast<RtspMediaSource>(_media_src)) {
        // Rtsp stream proxy
        if (reset_when_replay || !_muxer) {
            auto old = _option.enable_rtsp;
            _option.enable_rtsp = false;
            _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, getDuration(), _option);
            _muxer->setRecorderTimeFileReplay(_replay_recoder_time_file);
            _option.enable_rtsp = old;
        }
    } else if (dynamic_pointer_cast<RtmpMediaSource>(_media_src)) {
        // Rtmp stream proxy
        if (reset_when_replay || !_muxer) {
             auto old = _option.enable_rtmp;
            _option.enable_rtmp = false;
            _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, getDuration(), _option);
             _option.enable_rtmp = old;
        }
    } else {
        // Other stream proxies
        if (reset_when_replay || !_muxer) {
            _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, getDuration(), _option);
        }
    }
    _muxer->setMediaListener(shared_from_this());

    auto videoTrack = getTrack(TrackVideo, false);
    if (videoTrack) {
        // Add video
        _muxer->addTrack(videoTrack);
        // Write video data to _mediaMuxer
        videoTrack->addDelegate(_muxer);
    }

    auto audioTrack = getTrack(TrackAudio, false);
    if (audioTrack) {
        // Add audio
        _muxer->addTrack(audioTrack);
        // Write audio data to _mediaMuxer
        audioTrack->addDelegate(_muxer);
    }

    // After adding all tracks, prevent the maximum waiting time of 3 seconds in the case of a single track
    _muxer->addTrackCompleted();

    if (_media_src) {
        // Let the _muxer object intercept some events (such as recording related events)
        _media_src->setListener(_muxer);
    }
}

int PlayerProxy::getStatus() {
    return _live_status.load();
}
uint64_t PlayerProxy::getLiveSecs() {
    if (_live_status == 0) {
        return _live_secs + _live_ticker.elapsedTime() / 1000;
    }
    return _live_secs;
}

uint64_t PlayerProxy::getRePullCount() {
    return _repull_count;
}

} /* namespace mediakit */
