#ifndef S3MEDIAKIT_RTSPPUSHER_H
#define S3MEDIAKIT_RTSPPUSHER_H

#include <string>
#include <memory>
#include "RtspMediaSource.h"
#include "Poller/Timer.h"
#include "Network/Socket.h"
#include "Network/TcpClient.h"
#include "RtspSplitter.h"
#include "Pusher/PusherBase.h"
#include "Rtcp/RtcpContext.h"

namespace mediakit {

class RtspPusher : public toolkit::TcpClient, public RtspSplitter, public PusherBase {
public:
    using Ptr = std::shared_ptr<RtspPusher>;
    RtspPusher(const toolkit::EventPoller::Ptr &poller,const RtspMediaSource::Ptr &src);
    ~RtspPusher() override;
    void publish(const std::string &url) override;
    void teardown() override;

protected:
    //for Tcpclient override
    void onRecv(const toolkit::Buffer::Ptr &buf) override;
    void onConnect(const toolkit::SockException &err) override;
    void onError(const toolkit::SockException &ex) override;

    //RtspSplitter override
    void onWholeRtspPacket(Parser &parser) override ;
    void onRtpPacket(const char *data,size_t len) override;

    virtual void onRtcpPacket(int track_idx, SdpTrack::Ptr &track, uint8_t *data, size_t len);

private:
    void onPublishResult_l(const toolkit::SockException &ex, bool handshake_done);

    void sendAnnounce();
    void sendSetup(unsigned int track_idx);
    void sendRecord();
    void sendOptions();
    void sendTeardown();

    void handleResAnnounce(const Parser &parser);
    void handleResSetup(const Parser &parser, unsigned int track_idx);
    bool handleAuthenticationFailure(const std::string &params_str);

    int getTrackIndexByInterleaved(int interleaved) const;
    int getTrackIndexByTrackType(TrackType type) const;

    void sendRtpPacket(const RtspMediaSource::RingDataType & pkt) ;
    void sendRtspRequest(const std::string &cmd, const std::string &url ,const StrCaseMap &header = StrCaseMap(),const std::string &sdp = "" );
    void sendRtspRequest(const std::string &cmd, const std::string &url ,const std::initializer_list<std::string> &header,const std::string &sdp = "");

    void createUdpSockIfNecessary(int track_idx);
    void setSocketFlags();
    void updateRtcpContext(const RtpPacket::Ptr &pkt);

private:
    unsigned int _cseq = 1;
    Rtsp::eRtpType _rtp_type = Rtsp::RTP_TCP;

    // RTSP authentication related
    std::string _nonce;
    std::string _realm;
    std::string _url;
    std::string _session_id;
    std::string _content_base;
    SdpParser _sdp_parser;
    std::vector<SdpTrack::Ptr> _track_vec;
    // RTP port, trackid idx is the array index
    toolkit::Socket::Ptr _rtp_sock[2];
    // RTCP port, trackid idx is the array index
    toolkit::Socket::Ptr _rtcp_sock[2];
    // Timeout function implementation
    toolkit::Timer::Ptr _publish_timer;
    // Heartbeat timer
    toolkit::Timer::Ptr _beat_timer;
    std::weak_ptr<RtspMediaSource> _push_src;
    RtspMediaSource::RingType::RingReader::Ptr _rtsp_reader;
    std::function<void(const Parser&)> _on_res_func;
    ////////// rtcp ////////////////
    // RTCP send time, trackid idx is the array index
    toolkit::Ticker _rtcp_send_ticker[2];
    // Statistics RTP and send RTCP
    std::vector<RtcpContext::Ptr> _rtcp_context;
};

using RtspPusherImp = PusherImp<RtspPusher, PusherBase>;

} /* namespace mediakit */
#endif //S3MEDIAKIT_RTSPPUSHER_H
