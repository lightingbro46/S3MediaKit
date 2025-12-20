#include "WebRtcProxyPusher.h"
#include "Common/config.h"
#include "Http/HlsPlayer.h"
#include "Rtsp/RtspMediaSourceImp.h"
#include "WebRtcPlayer.h"

using namespace toolkit;
using namespace std;

namespace mediakit {

WebRtcProxyPusher::WebRtcProxyPusher(const EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src)
    : WebRtcClient(poller) {
    _push_src = src;
    DebugL;
}

WebRtcProxyPusher::~WebRtcProxyPusher(void) {
    teardown();
    DebugL;
}

void WebRtcProxyPusher::publish(const string &strUrl) {
    DebugL;
    try {
        _url.parse(strUrl, isPlayer());
    } catch (std::exception &ex) {
        onResult(SockException(Err_other, StrPrinter << "illegal webrtc url:" << ex.what()));
        return;
    }

    startConnect();
}

void WebRtcProxyPusher::teardown() {
    DebugL;
    _transport = nullptr;
}

void WebRtcProxyPusher::onResult(const SockException &ex) {
    DebugL << ex;
    if (!ex) {
        onPublishResult(ex);
    } else {
        if (!_is_negotiate_finished) {
            onPublishResult(ex);
        } else {
            onShutdown(ex);
        }
    }
}

float WebRtcProxyPusher::getTimeOutSec() {
    auto timeoutMS = (*this)[Client::kTimeoutMS].as<uint64_t>();
    return (float)timeoutMS / (float)1000;
}

void WebRtcProxyPusher::startConnect() {
    DebugL;
    MediaInfo info(_url._full_url);
    info.schema = "rtc";
    auto src = _push_src.lock();
    if (!src) {
        onResult(SockException(Err_other, "media source released"));
        return;
    }
    std::weak_ptr<WebRtcProxyPusher> weak_self = std::static_pointer_cast<WebRtcProxyPusher>(shared_from_this());
    _transport = WebRtcPlayer::create(getPoller(), src, info, WebRtcTransport::Role::CLIENT, _url._signaling_protocols);
    _transport->setOnShutdown([weak_self](const SockException &ex) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->onResult(ex);
        }
    });
    _transport->setOnStartWebRTC([weak_self]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->onResult(SockException());
        }
    });
    WebRtcClient::startConnect();
}

} /* namespace mediakit */
