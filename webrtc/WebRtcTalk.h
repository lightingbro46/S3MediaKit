#ifndef S3MEDIAKIT_WEBRTC_TALK_H
#define S3MEDIAKIT_WEBRTC_TALK_H

#include "WebRtcTransport.h"
#include "Rtsp/RtspMediaSource.h"
#include "Rtsp/RtspDemuxer.h"
#include "Rtp/RtpSender.h"

namespace mediakit {

class WebRtcTalk : public WebRtcTransportImp {
public:
    using Ptr = std::shared_ptr<WebRtcTalk>;
    static Ptr create(const toolkit::EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src, const MediaInfo &info,
                      WebRtcTransport::Role role, WebRtcTransport::SignalingProtocols signaling_protocols);

protected:
    ///////WebRtcTransportImp override///////
    void onStartWebRTC() override;
    void onDestory() override;
    void onRtcConfigure(RtcConfigure &configure) const override;
    void onRecvRtp(MediaTrack &track, const std::string &rid, RtpPacket::Ptr rtp) override;

private:
    WebRtcTalk(const toolkit::EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src, const MediaInfo &info);

private:
    // Media related metadata
    MediaInfo _media_info;
    // Playing rtsp source
    std::weak_ptr<RtspMediaSource> _play_src;

    // Reader object for playing rtsp source
    RtspMediaSource::RingType::RingReader::Ptr _reader;

    // Parse talk voice rtp stream to frame data
    RtspDemuxer::Ptr _demuxer;
    // Package voice frame data to specific rtp and reply back
    RtpSender::Ptr _sender;
};

}// namespace mediakit
#endif // S3MEDIAKIT_WEBRTC_TALK_H
