#ifndef S3MEDIAKIT_WEBRTC_CLIENT_H
#define S3MEDIAKIT_WEBRTC_CLIENT_H

#include "Http/HttpRequester.h"
#include "Sdp.h"
#include "WebRtcTransport.h"
#include "WebRtcSignalingPeer.h"
#include <memory>
#include <string>

namespace mediakit {

// Parsing utility class for WebRTC signaling URLs
class WebRTCUrl {
public:
    bool _is_ssl;
    std::string _full_url;
    std::string _negotiate_url; // for whep or whip
    std::string _delete_url; // for whep or whip
    std::string _target_secret;
    std::string _params;
    std::string _host;
    uint16_t _port;
    std::string _vhost;
    std::string _app;
    std::string _stream;
    WebRtcTransport::SignalingProtocols _signaling_protocols = WebRtcTransport::SignalingProtocols::WHEP_WHIP;
    std::string _peer_room_id; // peer room_id

public:
    void parse(const std::string &url, bool isPlayer);

private:
};

// Implements WebRTC proxy functionality
class WebRtcClient : public std::enable_shared_from_this<WebRtcClient> {
public:
    using Ptr = std::shared_ptr<WebRtcClient>;

    WebRtcClient(toolkit::EventPoller::Ptr poller);
    virtual ~WebRtcClient();

    const toolkit::EventPoller::Ptr &getPoller() const { return _poller; }
    void setPoller(toolkit::EventPoller::Ptr poller) { _poller = std::move(poller); }

    // Get WebRTC transport for API queries
    const WebRtcTransport::Ptr &getWebRtcTransport() const { return _transport; }

protected:
    virtual bool isPlayer() = 0;
    virtual void startConnect();
    virtual void onResult(const toolkit::SockException &ex) = 0;
    virtual void onNegotiateFinish();
    virtual float getTimeOutSec();

    void doNegotiate();
    void doNegotiateWebsocket();
    void doNegotiateWhepOrWhip();
    void checkIn();
    void doBye();
    void doByeWhepOrWhip();
    void checkOut();

    void gatheringCandidate(IceServerInfo::Ptr ice_server);
    void connectivityCheck();
    void candidate(const std::string &candidate, const std::string &ufrag, const std::string &pwd);

protected:
    toolkit::EventPoller::Ptr _poller;

    // for _negotiate_sdp
    WebRTCUrl _url;
    HttpRequester::Ptr _negotiate = nullptr;
    WebRtcSignalingPeer::Ptr _peer = nullptr;
    WebRtcTransport::Ptr _transport = nullptr;
    bool _is_negotiate_finished = false;

};

} /*namespace mediakit */
#endif /* S3MEDIAKIT_WEBRTC_CLIENT_H */
