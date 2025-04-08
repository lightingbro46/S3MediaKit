#include "WebRtcEchoTest.h"

namespace mediakit {

WebRtcEchoTest::Ptr WebRtcEchoTest::create(const EventPoller::Ptr &poller) {
    WebRtcEchoTest::Ptr ret(new WebRtcEchoTest(poller), [](WebRtcEchoTest *ptr) {
        ptr->onDestory();
        delete ptr;
    });
    ret->onCreate();
    return ret;
}

WebRtcEchoTest::WebRtcEchoTest(const EventPoller::Ptr &poller) : WebRtcTransportImp(poller) {}

void WebRtcEchoTest::onRtcConfigure(RtcConfigure &configure) const {
    WebRtcTransportImp::onRtcConfigure(configure);
    configure.audio.direction = configure.video.direction = RtpDirection::sendrecv;
    configure.audio.extmap.emplace(RtpExtType::sdes_mid, RtpDirection::sendrecv);
    configure.video.extmap.emplace(RtpExtType::sdes_mid, RtpDirection::sendrecv);
}

void WebRtcEchoTest::onRtp(const char *buf, size_t len, uint64_t stamp_ms) {
    updateTicker();
    sendRtpPacket(buf, len, true, nullptr);
}

void WebRtcEchoTest::onRtcp(const char *buf, size_t len) {
    sendRtcpPacket(buf, len, true, nullptr);
}

// Modify the a=msid attribute of mline, the purpose is that in the echo test case, if the offer and answer have the same msid, chrome will ignore the remote track.
void WebRtcEchoTest::onCheckSdp(SdpType type, RtcSession &sdp) {
    if (type == SdpType::answer) {
        for (auto &m : sdp.media) {
            for (auto &ssrc : m.rtp_rtx_ssrc) {
                if (!ssrc.msid.empty()) {
                    ssrc.msid = "s3mediakit-mslabel s3mediakit-label-" + m.mid;
                }
            }
        }
    }
}

}// namespace mediakit