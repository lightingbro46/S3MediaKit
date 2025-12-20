#ifndef S3MEDIAKIT_WEBRTC_PROXY_PLAYER_H
#define S3MEDIAKIT_WEBRTC_PROXY_PLAYER_H

#include "Network/Socket.h"
#include "Player/PlayerBase.h"
#include "Poller/Timer.h"
#include "Util/TimeTicker.h"
#include "WebRtcClient.h"
#include <memory>
#include <string>

namespace mediakit {

// Implements WebRTC proxy pulling functionality
class WebRtcProxyPlayer
    : public PlayerBase , public WebRtcClient {
public:
    using Ptr = std::shared_ptr<WebRtcProxyPlayer>;

    WebRtcProxyPlayer(const toolkit::EventPoller::Ptr &poller);
    ~WebRtcProxyPlayer() override;

    //// PlayerBase override////
    void play(const std::string &strUrl) override;
    void teardown() override;
    void pause(bool pause) override;
    void speed(float speed) override;

    std::shared_ptr<toolkit::SockInfo> getSockInfo() const override { 
        return getWebRtcTransport() ? getWebRtcTransport()->getSession() : nullptr;
    }
    size_t getRecvSpeed() override { 
        return getWebRtcTransport() ? getWebRtcTransport()->getRecvSpeed() : 0;
    }
    size_t getRecvTotalBytes() override { 
        return getWebRtcTransport() ? getWebRtcTransport()->getRecvTotalBytes() : 0; 
    }

protected:

    //// WebRtcClient override////
    bool isPlayer() override {return true;}
    float getTimeOutSec() override;
    void onNegotiateFinish() override;

protected:
    // Whether it is in benchmark mode
    bool _benchmark_mode = false;

    //Timeout function implementation
    toolkit::Ticker _recv_ticker;
    std::shared_ptr<toolkit::Timer> _check_timer;
};

} /* namespace mediakit */
#endif /* S3MEDIAKIT_WEBRTC_PROXY_PLAYER_H */
