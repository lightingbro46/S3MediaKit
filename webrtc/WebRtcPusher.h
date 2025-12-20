#ifndef S3MEDIAKIT_WEBRTCPUSHER_H
#define S3MEDIAKIT_WEBRTCPUSHER_H

#include "WebRtcTransport.h"
#include "Rtsp/RtspDemuxer.h"
#include "Rtsp/RtspMediaSource.h"

namespace mediakit {

class WebRtcPusher : public WebRtcTransportImp, public MediaSourceEvent {
public:
    using Ptr = std::shared_ptr<WebRtcPusher>;
    static Ptr create(const toolkit::EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src,
                      const std::shared_ptr<void> &ownership, const MediaInfo &info, const ProtocolOption &option, 
                      WebRtcTransport::Role role, WebRtcTransport::SignalingProtocols signaling_protocols);

protected:
    ///////WebRtcTransportImp override///////
    void onStartWebRTC() override;
    void onDestory() override;
    void onRtcConfigure(RtcConfigure &configure) const override;
    void onRecvRtp(MediaTrack &track, const std::string &rid, RtpPacket::Ptr rtp) override;
    void onShutdown(const toolkit::SockException &ex) override;
    void onRtcpBye() override;
    // //  dtls related callbacks ////
    void OnDtlsTransportClosed(const RTC::DtlsTransport *dtlsTransport) override;

protected:
    ///////MediaSourceEvent override///////
    // Close
    bool close(MediaSource &sender) override;
    // Total number of players
    int totalReaderCount(MediaSource &sender) override;
    // Get media source type
    MediaOriginType getOriginType(MediaSource &sender) const override;
    // Get media source url or file path
    std::string getOriginUrl(MediaSource &sender) const override;
    // Get media source client related information
    std::shared_ptr<toolkit::SockInfo> getOriginSock(MediaSource &sender) const override;
    // Due to support for discontinuous pushing, there is a possibility of OwnerPoller changes
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;
    // Get packet loss rate
    float getLossRate(MediaSource &sender,TrackType type) override;

private:
    WebRtcPusher(const toolkit::EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src,
                 const std::shared_ptr<void> &ownership, const MediaInfo &info, const ProtocolOption &option);

private:
    bool _simulcast = false;
    // Discontinuous pushing delay
    uint32_t _continue_push_ms = 0;
    // Media related metadata
    MediaInfo _media_info;
    // Rtsp source of the stream
    RtspMediaSource::Ptr _push_src;
    // Stream ownership
    std::shared_ptr<void> _push_src_ownership;
    // Rtsp source of the stream, supports simulcast
    std::recursive_mutex _mtx;
    std::unordered_map<std::string/*rid*/, RtspMediaSource::Ptr> _push_src_sim;
    std::unordered_map<std::string/*rid*/, std::shared_ptr<void> > _push_src_sim_ownership;
};

class WebRtcPlayerClient : public WebRtcTransportImp {
public:
    using Ptr = std::shared_ptr<WebRtcPlayerClient>;
    static Ptr create(const toolkit::EventPoller::Ptr &poller, WebRtcTransport::Role role, WebRtcTransport::SignalingProtocols signaling_protocols);

    void setMediaSource(RtspMediaSource::Ptr src);
    std::vector<Track::Ptr> getTracks(bool ready) const;

protected:
    ///////WebRtcTransportImp override///////
    void onStartWebRTC() override;
    void onRtcConfigure(RtcConfigure &configure) const override;
    void onRecvRtp(MediaTrack &track, const std::string &rid, RtpPacket::Ptr rtp) override;

private:
    WebRtcPlayerClient(const toolkit::EventPoller::Ptr &poller);

private:
    RtspDemuxer::Ptr _demuxer;
    // Rtsp source of the stream
    RtspMediaSource::Ptr _push_src;
};

}// namespace mediakit
#endif //S3MEDIAKIT_WEBRTCPUSHER_H
