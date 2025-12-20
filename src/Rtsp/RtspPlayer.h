#ifndef SRC_RTSPPLAYER_RTSPPLAYER_H_TXT_
#define SRC_RTSPPLAYER_RTSPPLAYER_H_TXT_

#include <string>
#include <memory>
#include "Util/TimeTicker.h"
#include "Poller/Timer.h"
#include "Network/Socket.h"
#include "Player/PlayerBase.h"
#include "Network/TcpClient.h"
#include "RtspSplitter.h"
#include "RtpReceiver.h"
#include "Rtcp/RtcpContext.h"

namespace mediakit {

// Implemented the rtsp player protocol part functionality, and data receiving functionality
class RtspPlayer : public PlayerBase, public toolkit::TcpClient, public RtspSplitter, public RtpReceiver {
public:
    using Ptr = std::shared_ptr<RtspPlayer>;

    RtspPlayer(const toolkit::EventPoller::Ptr &poller);
    ~RtspPlayer() override;

    void play(const std::string &strUrl) override;
    void pause(bool pause) override;
    void speed(float speed) override;
    void teardown() override;
    float getPacketLossRate(TrackType type) const override;

    size_t getRecvSpeed() override;
    size_t getRecvTotalBytes() override;

protected:
    // Derived class callback function
    virtual bool onCheckSDP(const std::string &sdp) = 0;
    virtual void onRecvRTP(RtpPacket::Ptr rtp, const SdpTrack::Ptr &track) = 0;
    uint32_t getProgressMilliSecond() const;
    void seekToMilliSecond(uint32_t ms);

    /**
     * Callback for receiving a complete rtsp packet, including sdp and other content data
     * @param parser rtsp packet
     */
    void onWholeRtspPacket(Parser &parser) override ;

    /**
     * Callback for receiving rtp packet
     * @param data
     * @param len
     */
    void onRtpPacket(const char *data,size_t len) override ;

    /**
     * Output rtp data packets after sorting
     * @param rtp rtp data packet
     * @param track_idx track index
     */
    void onRtpSorted(RtpPacket::Ptr rtp, int track_idx) override;

    /**
     * Parse out rtp but not yet sorted
     * @param rtp rtp data packet
     * @param track_index track index
     */
    void onBeforeRtpSorted(const RtpPacket::Ptr &rtp, int track_index) override;

    /**
     * Callback for receiving RTCP packet
     * @param track_idx track index
     * @param track sdp related information
     * @param data rtcp content
     * @param len rtcp content length
     */
    virtual void onRtcpPacket(int track_idx, SdpTrack::Ptr &track, uint8_t *data, size_t len);

    /////////////TcpClient override/////////////
    void onConnect(const toolkit::SockException &err) override;
    void onRecv(const toolkit::Buffer::Ptr &buf) override;
    void onError(const toolkit::SockException &ex) override;

private:
    void onPlayResult_l(const toolkit::SockException &ex , bool handshake_done);

    int getTrackIndexByPT(int pt) const;
    int getTrackIndexByInterleaved(int interleaved) const;
    int getTrackIndexByTrackType(TrackType track_type) const;

    void handleResSETUP(const Parser &parser, unsigned int track_idx);
    void handleResDESCRIBE(const Parser &parser);
    bool handleAuthenticationFailure(const std::string &wwwAuthenticateParamsStr);
    void handleResPAUSE(const Parser &parser, int type);
    using send_method_handler = void (RtspPlayer::*)(void);
    bool handleResponse(const std::string &cmd, const Parser &parser, send_method_handler handler);

    void sendOptions();
    void sendSetup(unsigned int track_idx);
    void sendPause(int type , uint32_t ms);
    void sendDescribe();
    void sendTeardown();
    void sendKeepAlive();
    void sendRtspRequest(const std::string &cmd, const std::string &url ,const StrCaseMap &header = StrCaseMap());
    void sendRtspRequest(const std::string &cmd, const std::string &url ,const std::initializer_list<std::string> &header);
    void createUdpSockIfNecessary(int track_idx);

private:
    // Whether it is performance test mode
    bool _benchmark_mode = false;
    // Send rtcp and GET_PARAMETER keep-alive in turn
    bool _send_rtcp[2] = {true, true};

    // Heartbeat type
    uint32_t _beat_type = 0;
    // Heartbeat protection interval
    uint32_t _beat_interval_ms = 0;

    std::string _play_url;
    // Rtsp start speed
    float _speed = 0.0f;
    std::vector<SdpTrack::Ptr> _sdp_track;
    std::function<void(const Parser&)> _on_response;
 protected:   
    // RTP port, trackid idx is the array subscript
    toolkit::Socket::Ptr _rtp_sock[2];
    // RTCP port, trackid idx is the array subscript
    toolkit::Socket::Ptr _rtcp_sock[2];

private:
    // Rtsp authentication related
    std::string _md5_nonce;
    std::string _realm;
    //rtsp info
    std::string _session_id;
    uint32_t _cseq_send = 1;
    std::string _content_base;
    std::string _control_url;
protected:   
    Rtsp::eRtpType _rtp_type = Rtsp::RTP_TCP;

private:
    // start timestamp
    uint64_t _first_stamp[2] = {0, 0};

    // Current rtp timestamp
    uint64_t _stamp[2] = {0, 0};

    // Timeout function implementation
    toolkit::Ticker _rtp_recv_ticker;
    std::shared_ptr<toolkit::Timer> _play_check_timer;
    std::shared_ptr<toolkit::Timer> _rtp_check_timer;
    // Server supported commands
    std::set<std::string> _supported_cmd;
    ////////// rtcp ////////////////
    // Rtcp send time, trackid idx is the array subscript
    toolkit::Ticker _rtcp_send_ticker[2];
    // Statistics rtp and send rtcp
    std::vector<RtcpContext::Ptr> _rtcp_context;
    // User-defined rtsp header
    StrCaseMap _custom_header;
};

} /* namespace mediakit */
#endif /* SRC_RTSPPLAYER_RTSPPLAYER_H_TXT_ */
