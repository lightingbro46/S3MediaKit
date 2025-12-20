#ifndef S3MEDIAKIT_WEBRTC_PROXY_PLAYER_IMP_H
#define S3MEDIAKIT_WEBRTC_PROXY_PLAYER_IMP_H

#include "WebRtcProxyPlayer.h"

namespace mediakit {

class WebRtcProxyPlayerImp
    : public PlayerImp<WebRtcProxyPlayer, PlayerBase>
    , private TrackListener {
public:
    using Ptr = std::shared_ptr<WebRtcProxyPlayerImp>;
    using Super = PlayerImp<WebRtcProxyPlayer, PlayerBase>;

    WebRtcProxyPlayerImp(const toolkit::EventPoller::Ptr &poller) : Super(poller) {}
    ~WebRtcProxyPlayerImp() override { DebugL; }

private:

    //// WebRtcProxyPlayer override////
    void startConnect() override;

    //// PlayerBase override////
    void onResult(const toolkit::SockException &ex) override;
    std::vector<Track::Ptr> getTracks(bool ready = true) const override;

    //// TrackListener override////
    bool addTrack(const Track::Ptr &track) override { return true; }
    void addTrackCompleted() override;
};

} /* namespace mediakit */
#endif /* S3MEDIAKIT_WEBRTC_PROXY_PLAYER_IMP_H */
