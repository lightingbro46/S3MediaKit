#include "SrtPlayer.h"
#include "SrtPlayerImp.h"
#include "Common/config.h"
#include "Http/HlsPlayer.h"

using namespace toolkit;
using namespace std;

namespace mediakit {


SrtPlayer::SrtPlayer(const EventPoller::Ptr &poller) 
    : SrtCaller(poller) {
    DebugL;
}

SrtPlayer::~SrtPlayer(void) {
    DebugL;
}

void SrtPlayer::play(const string &strUrl) {
    DebugL;
    try {
        _url.parse(strUrl);
    } catch (std::exception &ex) {
        onResult(SockException(Err_other, StrPrinter << "illegal srt url:" << ex.what()));
        return;
    }
    onConnect();
    return;
}

void SrtPlayer::teardown() {
    SrtCaller::onResult(SockException(Err_other, StrPrinter << "teardown: " << _url._full_url));
}

void SrtPlayer::pause(bool bPause) {
    DebugL;
}

void SrtPlayer::speed(float speed) {
    DebugL;
}

void SrtPlayer::onHandShakeFinished() {
    SrtCaller::onHandShakeFinished();
    onResult(SockException(Err_success, "srt play success"));
}

void SrtPlayer::onResult(const SockException &ex) {
    SrtCaller::onResult(ex);

     if (!ex) {
        // Play successfully
        onPlayResult(ex);
        _benchmark_mode = (*this)[Client::kBenchmarkMode].as<int>();

        // Play successfully, the packet reception timeout timer is restored
        _recv_ticker.resetTime();
        auto timeout = getTimeOutSec();
        //Read configuration files
        weak_ptr<SrtPlayer> weakSelf = static_pointer_cast<SrtPlayer>(shared_from_this());
        // Create a RTP data reception timeout detection timer
        _check_timer = std::make_shared<Timer>(timeout /2,
            [weakSelf, timeout]() {
                auto strongSelf = weakSelf.lock();
                if (!strongSelf) {
                    return false;
                }
                if (strongSelf->_recv_ticker.elapsedTime() > timeout * 1000) {
                    // Receive media packet timeout
                    strongSelf->onResult(SockException(Err_timeout, "receive srt media data timeout:" + strongSelf->_url._full_url));
                    return false;
                }

                return true;
            }, getPoller());
    } else {
        WarnL << ex.getErrCode() << " " << ex.what();
        if (ex.getErrCode() == Err_shutdown) {
            // Active shutdown, no callback triggering
            return;
        }
        if (!_is_handleshake_finished) {
            onPlayResult(ex);
        } else {
            onShutdown(ex);
        }
    }
    return;
}


void SrtPlayer::onSRTData(SRT::DataPacket::Ptr pkt) {
    _recv_ticker.resetTime();
}

uint16_t SrtPlayer::getLatency() {
    auto latency = (*this)[Client::kLatency].as<uint16_t>();
    return (uint16_t)latency ;
}

float SrtPlayer::getTimeOutSec() {
    auto timeoutMS = (*this)[Client::kTimeoutMS].as<uint64_t>();
    return (float)timeoutMS / (float)1000;
}

std::string SrtPlayer::getPassphrase() {
    auto passPhrase = (*this)[Client::kPassPhrase].as<string>();
    return passPhrase;
}

///////////////////////////////////////////////////
// SrtPlayerImp

void SrtPlayerImp::onPlayResult(const toolkit::SockException &ex) {
    if (ex) {
        Super::onPlayResult(ex);
    }
    //success result only occur when addTrackCompleted
    return;
}

std::vector<Track::Ptr> SrtPlayerImp::getTracks(bool ready /*= true*/) const {
    return _demuxer ? static_pointer_cast<HlsDemuxer>(_demuxer)->getTracks(ready) : Super::getTracks(ready);
}

void SrtPlayerImp::addTrackCompleted() {
    Super::onPlayResult(toolkit::SockException(toolkit::Err_success, "play success"));
}

void SrtPlayerImp::onSRTData(SRT::DataPacket::Ptr pkt) {
    SrtPlayer::onSRTData(pkt);

    if (_benchmark_mode) {
        return;
    }

    auto strong_self = shared_from_this();
    if (!_demuxer) {
        auto demuxer = std::make_shared<HlsDemuxer>();
        demuxer->start(getPoller(), this);
        _demuxer = std::move(demuxer);
    }

    if (!_decoder && _demuxer) {
        _decoder = DecoderImp::createDecoder(DecoderImp::decoder_ts, _demuxer.get());
    }

    if (_decoder && _demuxer) {
        _decoder->input(reinterpret_cast<const uint8_t *>(pkt->payloadData()), pkt->payloadSize());
    }

    return;
}


} /* namespace mediakit */

