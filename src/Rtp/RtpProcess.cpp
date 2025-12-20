#if defined(ENABLE_RTPPROXY)
#include "GB28181Process.h"
#include "RtpProcess.h"
#include "Util/File.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

// Before creating the _muxer object (before the streaming authentication is successful), you need to cache the frame first, which can prevent packet loss and improve the experience.
// But at the same time, you need to control the buffer length to prevent memory overflow. Caching 10 seconds of data should be enough to wait for the authentication hook to return.
static constexpr size_t kMaxCachedFrameMS = 10 * 1000;

namespace mediakit {

RtpProcess::Ptr RtpProcess::createProcess(const MediaTuple &tuple) {
    RtpProcess::Ptr ret(new RtpProcess(tuple));
    ret->createTimer();
    return ret;
}

RtpProcess::RtpProcess(const MediaTuple &tuple) {
    _media_info.schema = "rtp";
    static_cast<MediaTuple &>(_media_info) = tuple;

    GET_CONFIG(string, dump_dir, RtpProxy::kDumpDir);
    {
        FILE *fp = !dump_dir.empty() ? File::create_file(File::absolutePath(_media_info.stream + ".rtp", dump_dir), "wb") : nullptr;
        if (fp) {
            _save_file_rtp.reset(fp, [](FILE *fp) {
                fclose(fp);
            });
        }
    }

    {
        FILE *fp = !dump_dir.empty() ? File::create_file(File::absolutePath(_media_info.stream + ".video", dump_dir), "wb") : nullptr;
        if (fp) {
            _save_file_video.reset(fp, [](FILE *fp) {
                fclose(fp);
            });
        }
    }
}

void RtpProcess::flush() {
    if (_process) {
        _process->flush();
    }
}

RtpProcess::~RtpProcess() {
    uint64_t duration = (_last_frame_time.createdTime() - _last_frame_time.elapsedTime()) / 1000;
    WarnP(this) << "RTP stream pusher("
                << _media_info.shortUrl()
                << ") ddisconnected, time-consuming(s):" << duration;

    // Traffic statistics event broadcast
    GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
    if (_total_bytes >= iFlowThreshold * 1024) {
        try {
            NOTICE_EMIT(BroadcastFlowReportArgs, Broadcast::kBroadcastFlowReport, _media_info, _total_bytes, duration, false, *this);
        } catch (std::exception &ex) {
            WarnL << "Exception occurred: " << ex.what();
        }
    }
}

void RtpProcess::onManager() {
    if (!alive()) {
        onDetach(SockException(Err_timeout, "RtpProcess timeout"));
    }
}

void RtpProcess::createTimer() {
    // Create a timeout management timer
    weak_ptr<RtpProcess> weakSelf = shared_from_this();
    _timer = std::make_shared<Timer>(3.0f, [weakSelf] {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return false;
        }
        strongSelf->onManager();
        return true;
    }, EventPollerPool::Instance().getPoller());
}

bool RtpProcess::inputRtp(bool is_udp, const Socket::Ptr &sock, const char *data, size_t len, const struct sockaddr *addr, uint64_t *dts_out) {
    if (!isRtp(data, len)) {
        WarnP(this) << "Not rtp packet";
        return false;
    }
    if (!_auth_err.empty()) {
        throw toolkit::SockException(toolkit::Err_other, _auth_err);
    }
    auto header = (RtpHeader *) data;
    if (_sock != sock) {
        // First time running this function
        bool first = !_sock;
        _sock = sock;
        _addr.reset(new sockaddr_storage(*((sockaddr_storage *)addr)));
        if (first) {
            emitOnPublish(ntohl(header->ssrc));
            _cache_ticker.resetTime();
        }
    }

    _total_bytes += len;
    if (_save_file_rtp) {
        uint16_t size = (uint16_t)len;
        size = htons(size);
        fwrite((uint8_t *) &size, 2, 1, _save_file_rtp.get());
        fwrite((uint8_t *) data, len, 1, _save_file_rtp.get());
    }
    if (!_process) {
        _media_info.protocol = is_udp ? "udp" : "tcp";
        _process = std::make_shared<GB28181Process>(_media_info, this);
    }

    onRtp(ntohs(header->seq), ntohl(header->stamp),0/*Do not send sr, so it can be set to 0*/, 90000/*ps/ts stream timestamp according to 90K sampling rate*/, len);

    GET_CONFIG(string, dump_dir, RtpProxy::kDumpDir);
    if (_muxer && !_muxer->isEnabled() && !dts_out && dump_dir.empty()) {
        // When there is no access, and no timestamp is taken, and no debug file is exported, we can directly discard the data.
        _last_frame_time.resetTime();
        return false;
    }

    bool ret = _process ? _process->inputRtp(is_udp, data, len) : false;
    if (dts_out) {
        *dts_out = _dts;
    }
    return ret;
}

bool RtpProcess::inputFrame(const Frame::Ptr &frame) {
    _dts = frame->dts();
    if (_save_file_video && frame->getTrackType() == TrackVideo) {
        fwrite((uint8_t *) frame->data(), frame->size(), 1, _save_file_video.get());
    }
    if (_muxer) {
        _last_frame_time.resetTime();
        return _muxer->inputFrame(frame);
    }
    if (_cache_ticker.elapsedTime() > kMaxCachedFrameMS) {
        WarnL << "Cached frame of stream(" << _media_info.stream << ") is too much, your on_publish hook responded too late!";
        return false;
    }
    auto frame_cached = Frame::getCacheAbleFrame(frame);
    lock_guard<recursive_mutex> lck(_func_mtx);
    _cached_func.emplace_back([this, frame_cached]() {
        _last_frame_time.resetTime();
        _muxer->inputFrame(frame_cached);
    });
    return true;
}

bool RtpProcess::addTrack(const Track::Ptr &track) {
    if (_muxer) {
        return _muxer->addTrack(track);
    }

    lock_guard<recursive_mutex> lck(_func_mtx);
    _cached_func.emplace_back([this, track]() {
        _muxer->addTrack(track);
    });
    return true;
}

void RtpProcess::addTrackCompleted() {
    if (_muxer) {
        _muxer->addTrackCompleted();
    } else {
        lock_guard<recursive_mutex> lck(_func_mtx);
        _cached_func.emplace_back([this]() {
            _muxer->addTrackCompleted();
        });
    }
}

void RtpProcess::doCachedFunc() {
    lock_guard<recursive_mutex> lck(_func_mtx);
    for (auto &func : _cached_func) {
        func();
    }
    _cached_func.clear();
}

bool RtpProcess::alive() {
    if (_pause_timeout) {
        if (_last_check_alive.elapsedTime() < _pause_seconds * 1000) {
            return true;
        }
        // Pause rtp timeout detection for up to _pause_seconds seconds, because the NAT mapping validity period is generally not too long
        _pause_timeout = false;
    }

    _last_check_alive.resetTime();
    GET_CONFIG(uint64_t, timeoutSec, RtpProxy::kTimeoutSec)
    return _last_frame_time.elapsedTime() < timeoutSec * 1000;
}

void RtpProcess::pauseRtpTimeout(bool pause, uint32_t pause_seconds) {
    _pause_timeout = pause;
    // The default is 5 minutes to resume timeout monitoring
    _pause_seconds = pause_seconds ? pause_seconds : 300;
    if (!pause) {
        _last_frame_time.resetTime();
    }
}

void RtpProcess::setOnlyTrack(OnlyTrack only_track) {
    _only_track = only_track;
}

void RtpProcess::onDetach(const SockException &ex) {
    if (_on_detach) {
        WarnL << ex << ", stream_id: " << getIdentifier();
        _on_detach(ex);
    }
}

void RtpProcess::setOnDetach(onDetachCB cb) {
    _on_detach = std::move(cb);
}

string RtpProcess::get_peer_ip() {
    try {
        return _addr ? SockUtil::inet_ntoa((sockaddr *)_addr.get()) : "::";
    } catch (std::exception &ex) {
        return "::";
    }
}

uint16_t RtpProcess::get_peer_port() {
    try {
        return _addr ? SockUtil::inet_port((sockaddr *)_addr.get()) : 0;
    } catch (std::exception &ex) {
        return 0;
    }
}

string RtpProcess::get_local_ip() {
    return _sock ? _sock->get_local_ip() : "::";
}

uint16_t RtpProcess::get_local_port() {
    return _sock ? _sock->get_local_port() : 0;
}

string RtpProcess::getIdentifier() const {
    return _media_info.stream;
}

void RtpProcess::emitOnPublish(uint32_t ssrc) {
    weak_ptr<RtpProcess> weak_self = shared_from_this();
    Broadcast::PublishAuthInvoker invoker = [weak_self, ssrc](const string &err, const ProtocolOption &option) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        auto poller = strong_self->getOwnerPoller(MediaSource::NullMediaSource());
        poller->async([weak_self, err, option, ssrc]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            if (err.empty()) {
                strong_self->_muxer = std::make_shared<MultiMediaSourceMuxer>(strong_self->_media_info, 0.0f, option);
                switch (strong_self->_only_track) {
                    case kOnlyAudio: strong_self->_muxer->setOnlyAudio(); break;
                    case kOnlyVideo: strong_self->_muxer->enableAudio(false); break;
                    default: break;
                }
                strong_self->_muxer->setMediaListener(strong_self);
                strong_self->doCachedFunc();
                InfoP(strong_self) << "Allow RTP push streaming, ssrc: " << printSSRC(ssrc);
            } else {
                strong_self->_auth_err = err;
                WarnP(strong_self) << "Disable RTP push flow:" << err;
            }
        });
    };

    // Trigger the streaming authentication event
    auto flag = NOTICE_EMIT(BroadcastMediaPublishArgs, Broadcast::kBroadcastMediaPublish, MediaOriginType::rtp_push, _media_info, invoker, *this);
    if (!flag) {
        // No one is listening to this event, and authentication is not performed by default.
        invoker("", ProtocolOption());
    }
}

MediaOriginType RtpProcess::getOriginType(MediaSource &sender) const{
    return MediaOriginType::rtp_push;
}

string RtpProcess::getOriginUrl(MediaSource &sender) const {
    return _media_info.getUrl();
}

std::shared_ptr<SockInfo> RtpProcess::getOriginSock(MediaSource &sender) const {
    return const_cast<RtpProcess *>(this)->shared_from_this();
}

RtpProcess::Ptr RtpProcess::getRtpProcess(mediakit::MediaSource &sender) const {
    return const_cast<RtpProcess *>(this)->shared_from_this();
}

bool RtpProcess::close(mediakit::MediaSource &sender) {
    onDetach(SockException(Err_shutdown, "close media"));
    return true;
}

toolkit::EventPoller::Ptr RtpProcess::getOwnerPoller(MediaSource &sender) {
    if (_sock) {
        return _sock->getPoller();
    }
    throw std::runtime_error("RtpProcess::getOwnerPoller failed:" + _media_info.stream);
}

float RtpProcess::getLossRate(MediaSource &sender, TrackType type) {
    auto expected = getExpectedPacketsInterval();
    if (!expected) {
        return -1;
    }
    return getLostInterval() * 100 / expected;
}

const toolkit::Socket::Ptr& RtpProcess::getSock() const {
    return _sock;
}

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)