#include "FlvPlayer.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

FlvPlayer::FlvPlayer(const EventPoller::Ptr &poller) {
    setPoller(poller);
}

void FlvPlayer::play(const string &url) {
    TraceL << "play http-flv: " << url;
    _play_result = false;
    setProxyUrl((*this)[Client::kProxyUrl]);
    setHeaderTimeout((*this)[Client::kTimeoutMS].as<int>());
    setBodyTimeout((*this)[Client::kMediaTimeoutMS].as<int>());
    setMethod("GET");
    addCustomHeader(this);
    sendRequest(url);
}

void FlvPlayer::onResponseHeader(const string &status, const HttpClient::HttpHeader &header) {
    if (status != "200" && status != "206") {
        // HTTP status code does not meet expectations
        throw invalid_argument("bad http status code:" + status);
    }

    auto content_type = const_cast<HttpClient::HttpHeader &>(header)["Content-Type"];
    if (content_type.find("video/x-flv") != 0) {
        throw invalid_argument("content type not http-flv: " + content_type);
    }
}

void FlvPlayer::teardown() {
    HttpClientImp::shutdown();
}

void FlvPlayer::onResponseCompleted(const SockException &ex) {
    if (!_play_result) {
        _play_result = true;
        onPlayResult(ex);
    } else {
        onShutdown(ex);
    }
}

void FlvPlayer::onResponseBody(const char *buf, size_t size) {
    if (!_benchmark_mode) {
        // Performance test mode does not parse data to save CPU
        FlvSplitter::input(buf, size);
    }
}

bool FlvPlayer::onRecvMetadata(const AMFValue &metadata) {
    return onMetadata(metadata);
}

void FlvPlayer::onRecvRtmpPacket(RtmpPacket::Ptr packet) {
    if (!_play_result && !packet->isConfigFrame()) {
        _play_result = true;
        _benchmark_mode = (*this)[Client::kBenchmarkMode].as<int>();
        onPlayResult(SockException(Err_success, "play http-flv success"));
    }
    onRtmpPacket(std::move(packet));
}

size_t FlvPlayer::getRecvSpeed() {
    return TcpClient::getRecvSpeed();
}

size_t FlvPlayer::getRecvTotalBytes() {
    return TcpClient::getRecvTotalBytes();
}

}//mediakit