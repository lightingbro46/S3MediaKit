#ifndef SESSION_RTSPSESSION_H_
#define SESSION_RTSPSESSION_H_

#include <set>
#include <vector>
#include <unordered_set>
#include "Network/Session.h"
#include "RtspSplitter.h"
#include "RtpReceiver.h"
#include "Rtcp/RtcpContext.h"
#include "RtspMediaSource.h"
#include "RtspMediaSourceImp.h"
#include "RtpMultiCaster.h"

namespace mediakit {

using BufferRtp = toolkit::BufferOffset<toolkit::Buffer::Ptr>;
class RtspSession : public toolkit::Session, public RtspSplitter, public RtpReceiver, public MediaSourceEvent {
public:
    using Ptr = std::shared_ptr<RtspSession>;
    using onGetRealm = std::function<void(const std::string &realm)>;
    // `encrypted` being `true` indicates an MD5 encrypted password, otherwise it is a plain text password
    // When requesting a plain text password, providing an MD5 password will result in authentication failure
    using onAuth = std::function<void(bool encrypted, const std::string &pwd_or_md5)>;

    RtspSession(const toolkit::Socket::Ptr &sock);
    ////Session override////
    void onRecv(const toolkit::Buffer::Ptr &buf) override;
    void onError(const toolkit::SockException &err) override;
    void onManager() override;

protected:
    /////RtspSplitter override/////
    // Callback for receiving a complete RTSP packet, including SDP and other content data
    void onWholeRtspPacket(Parser &parser) override;
    // Callback for receiving an RTP packet
    void onRtpPacket(const char *data, size_t len) override;
    // Get the Content length from the RTSP header
    ssize_t getContentLength(Parser &parser) override;

    ////RtpReceiver override////
    void onRtpSorted(RtpPacket::Ptr rtp, int track_idx) override;
    void onBeforeRtpSorted(const RtpPacket::Ptr &rtp, int track_index) override;

    ///////MediaSourceEvent override///////
    // Close
    bool close(MediaSource &sender) override;
    // Total number of players
    int totalReaderCount(MediaSource &sender) override;
    // Get the media source type
    MediaOriginType getOriginType(MediaSource &sender) const override;
    // Get the media source URL or file path
    std::string getOriginUrl(MediaSource &sender) const override;
    // Get the media source client related information
    std::shared_ptr<SockInfo> getOriginSock(MediaSource &sender) const override;
    // Due to support for continuous pushing, there is a possibility of OwnerPoller changes
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;

    /////Session override////
    ssize_t send(toolkit::Buffer::Ptr pkt) override;
    // Callback for receiving an RTCP packet
    virtual void onRtcpPacket(int track_idx, SdpTrack::Ptr &track, const char *data, size_t len);

    // Reply to the client
    virtual bool sendRtspResponse(const std::string &res_code, const StrCaseMap &header = StrCaseMap(), const std::string &sdp = "", const char *protocol = "RTSP/1.0");

protected:
    // Information related to the URL after parsing
    MediaInfo _media_info;

    ////////RTP over udp_multicast////////
    // Shared RTP multicast object
    RtpMultiCaster::Ptr _multicaster;

    // Session number
    std::string _sessionid;

    uint32_t _multicast_ip = 0;
    uint16_t _multicast_video_port = 0;
    uint16_t _multicast_audio_port = 0;

private:
    // Handle the OPTIONS method, get server capabilities
    void handleReq_Options(const Parser &parser);
    // Handle the DESCRIBE method, request server RTSP SDP information
    void handleReq_Describe(const Parser &parser);
    // Handle the ANNOUNCE method, request streaming, with SDP attached
    void handleReq_ANNOUNCE(const Parser &parser);
    // Handle the RECORD method, start streaming
    void handleReq_RECORD(const Parser &parser);
    // Handle the SETUP method, used for negotiating RTP transport methods for playback and streaming
    void handleReq_Setup(const Parser &parser);
    // Handle the PLAY method, start or resume playback
    void handleReq_Play(const Parser &parser);
    // Handle the PAUSE method, pause playback
    void handleReq_Pause(const Parser &parser);
    // Handle the TEARDOWN method, end playback
    void handleReq_Teardown(const Parser &parser);
    // Handle the GET method, only used for RTP over HTTP
    void handleReq_Get(const Parser &parser);
    // Handle the POST method, only used for RTP over HTTP
    void handleReq_Post(const Parser &parser);
    // Handle the SET_PARAMETER, GET_PARAMETER methods, generally used for heartbeats
    void handleReq_SET_PARAMETER(const Parser &parser);
    // RTSP resource not found
    void send_StreamNotFound();
    // Unsupported transport mode
    void send_UnsupportedTransport();
    // Session ID error
    void send_SessionNotFound();
    // Triggered when the general RTSP server fails to open the port
    void send_NotAcceptable();
    // Get the track index
    int getTrackIndexByTrackType(TrackType type);
    int getTrackIndexByControlUrl(const std::string &control_url);
    int getTrackIndexByInterleaved(int interleaved);
    // Generally used to receive UDP hole punching packets, also used for RTSP pushing
    void onRcvPeerUdpData(int interleaved, const toolkit::Buffer::Ptr &buf, const struct sockaddr_storage &addr);
    // Used in conjunction with onRcvPeerUdpData
    void startListenPeerUdpData(int track_idx);
    // // RTSP specific authentication related ////
    // Authentication successful
    void onAuthSuccess();
    // Authentication failed
    void onAuthFailed(const std::string &realm, const std::string &why, bool close = true);
    // Start the RTSP specific authentication process
    void onAuthUser(const std::string &realm, const std::string &authorization);
    // Verify base64 authentication encryption
    void onAuthBasic(const std::string &realm, const std::string &auth_base64);
    // Verify MD5 authentication encryption
    void onAuthDigest(const std::string &realm, const std::string &auth_md5);
    // Trigger URL authentication event
    void emitOnPlay();
    // Send RTP to the client
    void sendRtpPacket(const RtspMediaSource::RingDataType &pkt);
    // Trigger RTCP sending
    void updateRtcpContext(const RtpPacket::Ptr &rtp);
    // Reply to the client
    bool sendRtspResponse(const std::string &res_code, const std::initializer_list<std::string> &header, const std::string &sdp = "", const char *protocol = "RTSP/1.0");

    // Set socket flag
    void setSocketFlags();

private:
    // Whether the on_play event has been triggered
    bool _emit_on_play = false;
    bool _send_sr_rtcp[2] = {true, true};
    // Delay in continuous pushing
    uint32_t _continue_push_ms = 0;
    // RTP transport method used by the pushing or pulling client
    Rtsp::eRtpType _rtp_type = Rtsp::RTP_Invalid;
    // Received seq, consistent when replying
    int _cseq = 0;
    // Total traffic consumed
    uint64_t _bytes_usage = 0;
    //ContentBase
    std::string _content_base;
    // Record whether RTSP specific authentication is required to prevent duplicate event triggering
    std::string _rtsp_realm;
    // Login authentication
    std::string _auth_nonce;
    // Used to determine if the client has timed out
    toolkit::Ticker _alive_ticker;

    // Source bound to RTSP pushing
    RtspMediaSourceImp::Ptr _push_src;
    // Pusher ownership
    std::shared_ptr<void> _push_src_ownership;
    // Live source bound to the RTSP player
    std::weak_ptr<RtspMediaSource> _play_src;
    // Live source reader
    RtspMediaSource::RingType::RingReader::Ptr _play_reader;
    // Valid track in SDP, including audio or video
    std::vector<SdpTrack::Ptr> _sdp_track;
    // Track specified by the player setup, default is TrackInvalid, which means no specification, both audio and video are pushed
    TrackType _target_play_track = TrackInvalid;

    ////////RTP over udp////////
    // RTP port, trackid idx is the array index
    toolkit::Socket::Ptr _rtp_socks[2];
    // RTCP port, trackid idx is the array index
    toolkit::Socket::Ptr _rtcp_socks[2];
    // Flag whether the UDP hole punching packet for playback has been received. The external UDP port number can only be known after receiving the UDP hole punching packet for playback.
    std::unordered_set<int> _udp_connected_flags;
    ////////RTSP over HTTP  ////////
    // QuickTime requests for RTSP will generate two TCP connections,
    // one for sending GET and one for sending POST. They need to be associated through x-sessioncookie.
    std::string _http_x_sessioncookie;
    std::function<void(const toolkit::Buffer::Ptr &)> _on_recv;
    ////////// rtcp ////////////////
    // RTCP send time, trackid idx is the array index
    toolkit::Ticker _rtcp_send_tickers[2];
    // Count RTP and send RTCP
    std::vector<RtcpContext::Ptr> _rtcp_context;
};

/**
 * RTSP server supporting SSL encryption, which can be used for devices such as Amazon Echo Show to access.
 */
using RtspSessionWithSSL = toolkit::SessionWithSSL<RtspSession>;

} /* namespace mediakit */

#endif /* SESSION_RTSPSESSION_H_ */
