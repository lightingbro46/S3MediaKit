#ifndef S3MEDIAKIT_RTPSENDER_H
#define S3MEDIAKIT_RTPSENDER_H
#if defined(ENABLE_RTPPROXY)
#include "PSEncoder.h"
#include "Extension/CommonRtp.h"
#include "Rtcp/RtcpContext.h"
#include "Common/MediaSource.h"
#include "Common/MediaSink.h"

namespace mediakit{

class RtpSession;

// RTP sending client, supporting sending GB28181 protocol
class RtpSender final : public MediaSinkInterface, public std::enable_shared_from_this<RtpSender>{
public:
    using Ptr = std::shared_ptr<RtpSender>;

    RtpSender(toolkit::EventPoller::Ptr poller = nullptr);
    ~RtpSender() override;

    /**
     * Start sending ps-rtp packets
     * @param args Sending parameters
     * @param cb Callback for whether the connection to the target port is successful
     */
    void startSend(const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t local_port, const toolkit::SockException &ex)> &cb);

    /**
     * Input frame data
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Refresh the output frame cache
     */
    void flush() override;

    /**
     * Add track, internally calls the clone method of Track
     * Only clones sps pps information, not Delegate relationships
     * @param track
     */
    virtual bool addTrack(const Track::Ptr & track) override;

    /**
     * All Tracks added
     */
    virtual void addTrackCompleted() override;

    /**
     * Reset track
     */
    virtual void resetTracks() override;

    /**
     * Set RTP sending stop callback
     */
    void setOnClose(std::function<void(const toolkit::SockException &ex)> on_close);

private:
    // Merge write output
    void onFlushRtpList(std::shared_ptr<toolkit::List<toolkit::Buffer::Ptr> > rtp_list);
    // UDP/TCP connection success callback
    void onConnect();
    // Abnormal socket disconnect event
    void onErr(const toolkit::SockException &ex);
    void createRtcpSocket();
    void onRecvRtcp(RtcpHeader *rtcp);
    void onSendRtpUdp(const toolkit::Buffer::Ptr &buf, bool check);
    void onClose(const toolkit::SockException &ex);

private:
    bool _is_connect = false;
    MediaSourceEvent::SendRtpArgs _args;
    toolkit::Socket::Ptr _socket_rtp;
    toolkit::Socket::Ptr _socket_rtcp;
    toolkit::EventPoller::Ptr _poller;
    MediaSinkInterface::Ptr _interface;
    std::shared_ptr<RtcpContext> _rtcp_context;
    toolkit::Ticker _rtcp_send_ticker;
    toolkit::Ticker _rtcp_recv_ticker;
    std::shared_ptr<RtpSession> _rtp_session;
    std::function<void(const toolkit::SockException &ex)> _on_close;
};

}//namespace mediakit
#endif// defined(ENABLE_RTPPROXY)
#endif //S3MEDIAKIT_RTPSENDER_H
