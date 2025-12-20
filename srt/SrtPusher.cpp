#include "SrtPusher.h"
#include "Common/config.h"

using namespace toolkit;
using namespace std;
namespace mediakit {

SrtPusher::SrtPusher(const EventPoller::Ptr &poller, const TSMediaSource::Ptr &src) : SrtCaller(poller) {
    _push_src = src;
    DebugL;
}

SrtPusher::~SrtPusher(void) {
    DebugL;
}

void SrtPusher::publish(const string &strUrl) {
    DebugL;
    try {
        _url.parse(strUrl);
    } catch (std::exception &ex) {
        onResult(SockException(Err_other, StrPrinter << "illegal srt url:" << ex.what()));
        return;
    }

    weak_ptr<SrtPusher> weak_self = static_pointer_cast<SrtPusher>(shared_from_this());
    getPoller()->async([weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->onConnect();
    });
    return;
}

void SrtPusher::teardown() {
    SrtCaller::onResult(SockException(Err_other, StrPrinter << "teardown: " << _url._full_url));
}

void SrtPusher::onHandShakeFinished() {
    SrtCaller::onHandShakeFinished();
    onResult(SockException(Err_success, "srt push success"));
    doPublish();
}

void SrtPusher::onResult(const SockException &ex) {
    SrtCaller::onResult(ex);

    if (!ex) {
        onPublishResult(ex);
    } else {
        WarnL << ex.getErrCode() << " " << ex.what();
        if (ex.getErrCode() == Err_shutdown) {
            // Active shutdown, no callback triggering
            return;
        }
        if (!_is_handleshake_finished) {
            onPublishResult(ex);
        } else {
            onShutdown(ex);
        }
    }
    return;
}

uint16_t SrtPusher::getLatency() {
    auto latency = (*this)[Client::kLatency].as<uint16_t>();
    return (uint16_t)latency ;
}

float SrtPusher::getTimeOutSec() {
    auto timeoutMS = (*this)[Client::kTimeoutMS].as<uint64_t>();
    return (float)timeoutMS / (float)1000;
}

std::string SrtPusher::getPassphrase() {
    auto passPhrase = (*this)[Client::kPassPhrase].as<string>();
    return passPhrase;
}

void SrtPusher::doPublish() {
    auto src = _push_src.lock();
    if (!src) {
        onResult(SockException(Err_eof, "the media source was released"));
        return;
    }
    // Asynchronously search for live streams
    std::weak_ptr<SrtPusher> weak_self = static_pointer_cast<SrtPusher>(shared_from_this());
    _ts_reader = src->getRing()->attach(getPoller());
    _ts_reader->setDetachCB([weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            // This object has been destroyed
            return;
        }
        strong_self->onShutdown(SockException(Err_shutdown));
    });
    _ts_reader->setReadCB([weak_self](const TSMediaSource::RingDataType &ts_list) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            // This object has been destroyed
            return;
        }
        size_t i = 0;
        auto size = ts_list->size();
        ts_list->for_each([&](const TSPacket::Ptr &ts) { 
            strong_self->onSendTSData(ts, ++i == size); 
        });
    });
}

size_t SrtPusher::getSendSpeed() {
    return SrtCaller::getSendSpeed();
}

size_t SrtPusher::getSendTotalBytes() {
    return SrtCaller::getSendTotalBytes();
}

} /* namespace mediakit */

