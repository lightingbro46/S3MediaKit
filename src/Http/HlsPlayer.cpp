#include "HlsPlayer.h"
#include "Common/config.h"
using namespace std;
using namespace toolkit;

namespace mediakit {

HlsPlayer::HlsPlayer(const EventPoller::Ptr &poller) {
    setPoller(poller ? poller : EventPollerPool::Instance().getPoller());
}

void HlsPlayer::play(const string &url) {
    _play_result = false;
    _play_url = url;
    setProxyUrl((*this)[Client::kProxyUrl]);
    setAllowResendRequest(true);
    fetchIndexFile();
}

void HlsPlayer::fetchIndexFile() {
    if (waitResponse()) {
        return;
    }
    if (!(*this)[Client::kNetAdapter].empty()) {
        setNetAdapter((*this)[Client::kNetAdapter]);
    }
    setCompleteTimeout((*this)[Client::kTimeoutMS].as<int>());
    setMethod("GET");
    addCustomHeader(this);
    sendRequest(_play_url);
}

void HlsPlayer::teardown_l(const SockException &ex) {
    if (!_play_result) {
        _play_result = true;
        onPlayResult(ex);
    } else {
        // If it is not actively closed, then re-pull the index file
        // if not actively closed, re-fetch the index file
        if (ex.getErrCode() != Err_shutdown && HlsParser::isLive()) {
            // If the retry count has reached the maximum number of times, and the slice list is empty, and there are no slices being downloaded, then it is considered a failure to close the player
            // If the retry count has reached the maximum number of times, and the segments list is empty, and there is no segment being downloaded,
            // the player is considered to be closed due to failure
            if (_ts_list.empty() && !(_http_ts_player && _http_ts_player->waitResponse()) && _try_fetch_index_times >= MAX_TRY_FETCH_INDEX_TIMES) {
                onShutdown(ex);
            } else {
                _try_fetch_index_times += 1;
                shutdown(ex);
                WarnL << "Attempt to pull the m3u8 file again[" << _try_fetch_index_times << "]:" << _play_url;
                // When the network fluctuates, it is possible that the m3u8 file will fail to be pulled, so quickly retry pulling the m3u8 file instead of directly closing the player
                // A delay is added here to prevent the _http_ts_player socket from remaining alive and pulling the m3u8 file multiple times
                // When the network fluctuates, it is possible to fail to pull the m3u8 file, so quickly retry to pull the m3u8 file instead of closing the player directly
                // The delay here is to prevent the socket of _http_ts_player from still keeping alive state, and pull the m3u8 file multiple times
                // todo Is the _http_ts_player->waitResponse() condition necessary? Because sometimes there is _complete==true, but _http_ts_player->alive() is true
                playDelay(0.3);
                return;
            }
        } else {
            onShutdown(ex);
        }
    }
    _timer.reset();
    _timer_ts.reset();
    _http_ts_player.reset();
    shutdown(ex);
}

void HlsPlayer::teardown() {
    teardown_l(SockException(Err_shutdown, "teardown"));
}

void HlsPlayer::fetchSegment() {
    if (_ts_list.empty()) {
        // If it is an on-demand file, an empty playlist means that the file playback is finished, and the player is closed: #2628
        // If it is a video-on-demand file, the playlist is empty means the file is finished playing, close the player: #2628
        if (!HlsParser::isLive()) {
            teardown();
            return;
        }
        // If the playlist is empty, then immediately re-download the m3u8 file
        // The playlist is empty, so download the m3u8 file immediately
        _timer.reset();
        fetchIndexFile();
        return;
    }
    if (_http_ts_player && _http_ts_player->waitResponse()) {
        // The player is still alive and is currently downloading
        return;
    }
    weak_ptr<HlsPlayer> weak_self = static_pointer_cast<HlsPlayer>(shared_from_this());
    if (!_http_ts_player) {
        _http_ts_player = std::make_shared<HttpTSPlayer>(getPoller());
        _http_ts_player->setProxyUrl((*this)[Client::kProxyUrl]);
        _http_ts_player->setAllowResendRequest(true);
        _http_ts_player->setOnCreateSocket([weak_self](const EventPoller::Ptr &poller) {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                return strong_self->createSocket();
            }
            return Socket::createSocket(poller, true);
        });
        auto benchmark_mode = (*this)[Client::kBenchmarkMode].as<int>();
        if (!benchmark_mode) {
            _http_ts_player->setOnPacket([weak_self](const char *data, size_t len) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                // Received ts packet
                // Received ts package
                strong_self->onPacket(data, len);
            });
        }

        if (!(*this)[Client::kNetAdapter].empty()) {
            _http_ts_player->setNetAdapter((*this)[Client::kNetAdapter]);
        }
    } else {
        // Reset HttpTSPlayer state every time new ts fragment is requested
        _http_ts_player->clear();
        _http_ts_player->setProxyUrl((*this)[Client::kProxyUrl]);
    }

    Ticker ticker;
    auto url = _ts_list.front().url;
    auto duration = _ts_list.front().duration;
    _ts_list.pop_front();

    _http_ts_player->setOnComplete([weak_self, ticker, duration, url](const SockException &err) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        if (err) {
            WarnL << "Download ts segment " << url << " failed:" << err;
            if (err.getErrCode() == Err_timeout) {
                strong_self->_timeout_multiple = MAX(strong_self->_timeout_multiple + 1, MAX_TIMEOUT_MULTIPLE);
            } else {
                strong_self->_timeout_multiple = MAX(strong_self->_timeout_multiple - 1, MIN_TIMEOUT_MULTIPLE);
            }
            strong_self->_ts_download_failed_count++;
            if (strong_self->_ts_download_failed_count > MAX_TS_DOWNLOAD_FAILED_COUNT) {
                WarnL << "ts segment " << url << " download failed count is " << strong_self->_ts_download_failed_count << ", teardown player";
                strong_self->teardown_l(SockException(Err_shutdown, "ts segment download failed"));
                return;
            }
        } else {
            strong_self->_ts_download_failed_count = 0;
        }
        // Download 0.5 seconds in advance to support on-demand file download speed control: #2628
        // Download 0.5 seconds in advance to support video-on-demand files to control download speed: #2628
        auto delay = duration - 0.5 - ticker.elapsedTime() / 1000.0f;
        if (delay > 2.0) {
            // Download 1 second in advance
            // Download 1 second in advance
            delay -= 1.0;
        } else if (delay <= 0) {
            // Delay a minimum of 10ms
            // Delay at least 10ms
            delay = 0.01;
        }
        // Delay downloading the next slice
        strong_self->_timer_ts.reset(new Timer(delay, [weak_self]() {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                strong_self->fetchSegment();
            }
            return false;
        }, strong_self->getPoller()));
    });

    _http_ts_player->setMethod("GET");
    // The ts slice must be downloaded within 2-5 times its duration
    // The ts segment must be downloaded within 2-5 times its duration
    _http_ts_player->setCompleteTimeout(_timeout_multiple * duration * 1000);
    _http_ts_player->sendRequest(url);
}

bool HlsPlayer::onParsed(bool is_m3u8_inner, int64_t sequence, const map<int, ts_segment> &ts_map) {
    if (!is_m3u8_inner) {
        // This is the ts playlist
        // This is the ts playlist
        if (_last_sequence == sequence) {
            // If it is a duplicate ts list, then ignore it
            // However, it should be noted that if the current ts list is empty, then it means that the live broadcast has ended or the m3u8 file has a problem, and the stream needs to be re-pulled
            // The 5 times here is to prevent infinite retries caused by problems with the m3u8 file
            // If it is a duplicate ts list, ignore it
            // But it should be noted that if the current ts list is empty, it means that the live broadcast is over or the m3u8 file is problematic, and you need to re-pull the stream
            // The 5 times here is to prevent infinite retries caused by problems with the m3u8 file
            if (_last_sequence > 0 && _ts_list.empty() && HlsParser::isLive()
                && _wait_index_update_ticker.elapsedTime() > (uint64_t)HlsParser::getTargetDur() * 1000 * 5) {
                _wait_index_update_ticker.resetTime();
                WarnL << "Fetch new ts list from m3u8 timeout";
                return false;
            }
            return true;
        }

        _last_sequence = sequence;
        _wait_index_update_ticker.resetTime();
        for (auto &pr : ts_map) {
            auto &ts = pr.second;
            if (_ts_url_cache.emplace(ts.url).second) {
                // This ts is not duplicated
                // The ts is not repeated
                _ts_list.emplace_back(ts);
                // Sort by time
                // Sort by time
                _ts_url_sort.emplace_back(ts.url);
            }
        }
        if (_ts_url_sort.size() > 2 * ts_map.size()) {
            // Remove excessive data from the anti-repetition list
            // Remove too much data from the anti-repetition list
            _ts_url_cache.erase(_ts_url_sort.front());
            _ts_url_sort.pop_front();
        }
        fetchSegment();
    } else {
        // This is the m3u8 list, we play the highest definition sub-hls
        // This is the m3u8 list, we play the highest quality sub-hls
        if (ts_map.empty()) {
            throw invalid_argument("empty sub hls list:" + getUrl());
        }
        _timer.reset();
        weak_ptr<HlsPlayer> weak_self = static_pointer_cast<HlsPlayer>(shared_from_this());
        auto url = ts_map.rbegin()->second.url;
        getPoller()->async([weak_self, url]() {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                strong_self->play(url);
            }
        }, false);
    }
    return true;
}

void HlsPlayer::onResponseHeader(const string &status, const HttpClient::HttpHeader &headers) {
    if (status != "200" && status != "206") {
        // Failure
        // Failed
        throw invalid_argument("bad http status code:" + status);
    }
    auto content_type = strToLower(const_cast<HttpClient::HttpHeader &>(headers)["Content-Type"]);
    if (content_type.find("application/vnd.apple.mpegurl") != 0 && content_type.find("/x-mpegurl") == _StrPrinter::npos) {
        WarnL << "May not a hls video: " << content_type << ", url: " << getUrl();
    }
    _m3u8.clear();
}

void HlsPlayer::onResponseBody(const char *buf, size_t size) {
    _m3u8.append(buf, size);
    _recvtotalbytes += getRecvTotalBytes();
}

void HlsPlayer::onResponseCompleted(const SockException &ex) {
    if (ex) {
        teardown_l(ex);
        return;
    }
    if (!HlsParser::parse(getUrl(), _m3u8)) {
        teardown_l(SockException(Err_other, "parse m3u8 failed:" + _play_url));
        return;
    }
    // If there are or new slices are obtained, then it is considered successful, and the failure count should be reset
    // if there are new segments or get new segments, it is considered successful, and the number of failures should be reset
    if (!_ts_list.empty()) {
        _try_fetch_index_times = 0;
    }
    if (!_play_result) {
        _play_result = true;
        onPlayResult(SockException());
    }
    playDelay();
}

float HlsPlayer::delaySecond() {
    if (HlsParser::isM3u8() && HlsParser::getTargetDur() > 0) {
        float targetOffset;
        if (HlsParser::isLive()) {
            // see RFC 8216, Section 4.4.3.8.
            // According to the rfc, the refresh cycle of the index list should be 3 times the segment time, because according to the specification, the player only processes the last 3 Segments
            // refresh the index list according to rfc cycle should be the segment time x3,
            // because according to the specification, the player only handles the last 3 segments
            targetOffset = (float)(3 * HlsParser::getTargetDur());
        } else {
            // On-demand generally does not change the m3u8 file, there is no need to refresh frequently, so refresh according to the total time
            // On-demand, the m3u8 file will generally not change, so there is no need to refresh frequently,
            targetOffset = HlsParser::getTotalDuration();
        }
        // Take the minimum value to avoid problems caused by irregular segment durations
        // Take the minimum value to avoid problems caused by irregular segment duration
        if (targetOffset > HlsParser::getTotalDuration()) {
            targetOffset = HlsParser::getTotalDuration();
        }
        // According to the specification, it is half the time
        // According to the specification, it is half the time
        if (targetOffset / 2 > 1.0f) {
            return targetOffset / 2;
        }
    }
    return 1.0f;
}

bool HlsPlayer::onRedirectUrl(const string &url, bool temporary) {
    _play_url = url;
    return true;
}

void HlsPlayer::playDelay(float delay_sec) {
    weak_ptr<HlsPlayer> weak_self = static_pointer_cast<HlsPlayer>(shared_from_this());
    if (delay_sec == 0) {
        delay_sec = delaySecond();
    }
    _timer.reset(new Timer(delay_sec, [weak_self]() {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                strong_self->fetchIndexFile();
            }
            return false;
        }, getPoller()));
}

size_t HlsPlayer::getRecvSpeed() {
    return TcpClient::getRecvSpeed() + (_http_ts_player ? _http_ts_player->getRecvSpeed() : 0);
}

size_t HlsPlayer::getRecvTotalBytes() {
    return TcpClient::getRecvTotalBytes() + (_http_ts_player ? _http_ts_player->getRecvTotalBytes() : 0);
}
//////////////////////////////////////////////////////////////////////////

void HlsDemuxer::start(const EventPoller::Ptr &poller, TrackListener *listener) {
    _frame_cache.clear();
    _delegate.setTrackListener(listener);

    // Execute once every 50 milliseconds
    // Execute every 50 milliseconds
    weak_ptr<HlsDemuxer> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(0.05f, [weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }
        strong_self->onTick();
        return true;
    }, poller);
}

void HlsDemuxer::pushTask(std::function<void()> task) {
    int64_t stamp = 0;
    if (!_frame_cache.empty()) {
        stamp = _frame_cache.back().first;
    }
    _frame_cache.emplace_back(std::make_pair(stamp, std::move(task)));
}

bool HlsDemuxer::inputFrame(const Frame::Ptr &frame) {
    // To avoid the track preparation time being too long, all frames are directly consumed before it is ready
    // In order to avoid the track preparation time is too long, so before it is ready, all frames are consumed directly
    if (!_delegate.isAllTrackReady()) {
        _delegate.inputFrame(frame);
        return true;
    }

    if (_frame_cache.empty()) {
        // Set the current playback position timestamp
        // Set the current playback position timestamp
        setPlayPosition(frame->dts());
    }
    // Cache frames based on the timestamp
    // Cache frame according to timestamp
    auto cached_frame = Frame::getCacheAbleFrame(frame);
    _frame_cache.emplace_back(std::make_pair(frame->dts(), [cached_frame, this]() {
        _delegate.inputFrame(cached_frame);
    }));

    if (getBufferMS() > 30 * 1000) {
        // If the cache exceeds 30 seconds, force consumption to 15 seconds (reduce latency or memory usage)
        // The cache exceeds 30 seconds, and the consumption is forced to 15 seconds (reduce delay or memory usage)
        while (getBufferMS() > 15 * 1000) {
            _frame_cache.begin()->second();
            _frame_cache.erase(_frame_cache.begin());
        }
        // Then play the earliest frame in the cache
        // Then play the earliest frame in the cache
        setPlayPosition(_frame_cache.begin()->first);
    }
    return true;
}

int64_t HlsDemuxer::getPlayPosition() {
    return _ticker.elapsedTime() + _ticker_offset;
}

int64_t HlsDemuxer::getBufferMS() {
    if (_frame_cache.empty()) {
        return 0;
    }
    return _frame_cache.rbegin()->first - _frame_cache.begin()->first;
}

void HlsDemuxer::setPlayPosition(int64_t pos) {
    _ticker.resetTime();
    _ticker_offset = pos;
}

void HlsDemuxer::onTick() {
    auto it = _frame_cache.begin();
    while (it != _frame_cache.end()) {
        if (it->first > getPlayPosition()) {
            // These frames have not yet reached their playback time
            // These frames are not yet time to play
            break;
        }

        if (getBufferMS() < 3 * 1000) {
            // If the cache is less than 3 seconds, then reduce the timer consumption speed (so that the remaining data is consumed after 3 seconds)
            // The goal is to prevent the timer from waiting for a long time before the data is consumed instantly
            // If the cache is less than 3 seconds, then reduce the speed of the timer to consume (let the remaining data be consumed after 3 seconds)
            // The purpose is to prevent the timer from waiting for a long time, and the data is consumed instantly
            setPlayPosition(_frame_cache.begin()->first);
        }

        // Consume expired frames
        // Consume expired frames
        it->second();
        it = _frame_cache.erase(it);
    }
}

//////////////////////////////////////////////////////////////////////////

HlsPlayerImp::HlsPlayerImp(const EventPoller::Ptr &poller) : PlayerImp<HlsPlayer, PlayerBase>(poller) {}

void HlsPlayerImp::onPacket(const char *data, size_t len) {
    if (!_decoder && _demuxer) {
        _decoder = DecoderImp::createDecoder(DecoderImp::decoder_ts, _demuxer.get());
    }

    if (_decoder && _demuxer) {
        _decoder->input((uint8_t *) data, len);
    }
    _recvtotalbytes += HlsPlayer::getRecvTotalBytes();
}

void HlsPlayerImp::addTrackCompleted() {
    PlayerImp<HlsPlayer, PlayerBase>::onPlayResult(SockException(Err_success, "play hls success"));
}

void HlsPlayerImp::onPlayResult(const SockException &ex) {
    auto benchmark_mode = (*this)[Client::kBenchmarkMode].as<int>();
    if (ex || benchmark_mode) {
        PlayerImp<HlsPlayer, PlayerBase>::onPlayResult(ex);
    } else {
        auto demuxer = std::make_shared<HlsDemuxer>();
        demuxer->start(getPoller(), this);
        _demuxer = std::move(demuxer);
    }
}

void HlsPlayerImp::onShutdown(const SockException &ex) {
    while (_demuxer) {
        try {
            // shared_from_this() may throw an exception
            // shared_from_this() may throw an exception
            std::weak_ptr<HlsPlayerImp> weak_self = static_pointer_cast<HlsPlayerImp>(shared_from_this());
            if (_decoder) {
                _decoder->flush();
            }
            // Wait for all frames to be flushed before triggering the onShutdown event
            // Wait for all frame flush output, then trigger the onShutdown event
            static_pointer_cast<HlsDemuxer>(_demuxer)->pushTask([weak_self, ex]() {
                if (auto strong_self = weak_self.lock()) {
                    strong_self->_demuxer = nullptr;
                    strong_self->onShutdown(ex);
                }
            });
            return;
        } catch (...) {
            break;
        }
    }
    PlayerImp<HlsPlayer, PlayerBase>::onShutdown(ex);
}

vector<Track::Ptr> HlsPlayerImp::getTracks(bool ready) const {
    if (!_demuxer) {
        return vector<Track::Ptr>();
    }
    return static_pointer_cast<HlsDemuxer>(_demuxer)->getTracks(ready);
}

size_t HlsPlayerImp::getRecvSpeed() {
    return PlayerImp<HlsPlayer, PlayerBase>::getRecvSpeed();
}

size_t HlsPlayerImp::getRecvTotalBytes() {
     return _recvtotalbytes;
}
}//namespace mediakit
