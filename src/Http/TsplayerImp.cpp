#include "TsPlayerImp.h"
#include "HlsPlayer.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

TsPlayerImp::TsPlayerImp(const EventPoller::Ptr &poller) : PlayerImp<TsPlayer, PlayerBase>(poller) {}

void TsPlayerImp::onResponseBody(const char *data, size_t len) {
    TsPlayer::onResponseBody(data, len);
    if (!_decoder && _demuxer) {
        _decoder = DecoderImp::createDecoder(DecoderImp::decoder_ts, _demuxer.get());
    }

    if (_decoder && _demuxer) {
        _decoder->input((uint8_t *) data, len);
    }
}

void TsPlayerImp::addTrackCompleted() {
    PlayerImp<TsPlayer, PlayerBase>::onPlayResult(SockException(Err_success, "play http-ts success"));
}

void TsPlayerImp::onPlayResult(const SockException &ex) {
    auto benchmark_mode = (*this)[Client::kBenchmarkMode].as<int>();
    if (ex || benchmark_mode) {
        PlayerImp<TsPlayer, PlayerBase>::onPlayResult(ex);
    } else {
        auto demuxer = std::make_shared<HlsDemuxer>();
        demuxer->start(getPoller(), this);
        _demuxer = std::move(demuxer);
    }
}

void TsPlayerImp::onShutdown(const SockException &ex) {
    while (_demuxer) {
        try {
            // shared_from_this() may throw an exception
            std::weak_ptr<TsPlayerImp> weak_self = static_pointer_cast<TsPlayerImp>(shared_from_this());
            if (_decoder) {
                _decoder->flush();
            }
            // Wait for all frame flush output before triggering the onShutdown event
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
    PlayerImp<TsPlayer, PlayerBase>::onShutdown(ex);
}

vector<Track::Ptr> TsPlayerImp::getTracks(bool ready) const {
    if (!_demuxer) {
        return vector<Track::Ptr>();
    }
    return static_pointer_cast<HlsDemuxer>(_demuxer)->getTracks(ready);
}

}//namespace mediakit