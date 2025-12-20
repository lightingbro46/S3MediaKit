#ifndef S3MEDIAKIT_WEBRTCECHOTEST_H
#define S3MEDIAKIT_WEBRTCECHOTEST_H

#include "WebRtcTransport.h"

namespace mediakit {

class WebRtcEchoTest : public WebRtcTransportImp {
public:
    using Ptr = std::shared_ptr<WebRtcEchoTest>;
    static Ptr create(const toolkit::EventPoller::Ptr &poller);

protected:
    ///////WebRtcTransportImp override///////
    void onRtcConfigure(RtcConfigure &configure) const override;
    void onCheckSdp(SdpType type, RtcSession &sdp) override;
    void onRtp(const char *buf, size_t len, uint64_t stamp_ms) override;
    void onRtcp(const char *buf, size_t len) override;

    void onBeforeEncryptRtp(const char *buf, int &len, void *ctx) override {};
    void onBeforeEncryptRtcp(const char *buf, int &len, void *ctx) override {};

private:
    WebRtcEchoTest(const toolkit::EventPoller::Ptr &poller);
};

}// namespace mediakit
#endif //S3MEDIAKIT_WEBRTCECHOTEST_H
