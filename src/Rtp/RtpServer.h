#ifndef S3MEDIAKIT_RTPSERVER_H
#define S3MEDIAKIT_RTPSERVER_H

#if defined(ENABLE_RTPPROXY)
#include <memory>
#include "Network/Socket.h"
#include "Network/TcpServer.h"
#include "Network/UdpServer.h"
#include "RtpSession.h"

namespace mediakit {

class RtcpHelper;

/**
 * RTP server, supports UDP/TCP
 */
class RtpServer : public std::enable_shared_from_this<RtpServer> {
public:
    using Ptr = std::shared_ptr<RtpServer>;
    using onRecv = std::function<void(const toolkit::Buffer::Ptr &buf)>;
    enum TcpMode { NONE = 0, PASSIVE, ACTIVE };

    ~RtpServer();

    /**
     * Start the server, may throw an exception
     * @param local_port Local port, 0 for random port
     * @param local_ip Local network interface ip to bind
     * @param stream_id Stream id, use ssrc if empty
     * @param tcp_mode TCP service mode
     * @param re_use_port Whether to set the socket to re_use property
     * @param ssrc Specified ssrc
     * @param multiplex Multiplexing
     */
    void start(uint16_t local_port, const char *local_ip = "::", const MediaTuple &tuple = MediaTuple{DEFAULT_VHOST, kRtpAppName, "", ""}, TcpMode tcp_mode = PASSIVE,
               bool re_use_port = true, uint32_t ssrc = 0, int only_track = 0, bool multiplex = false);

    /**
     * Connect to the tcp service (tcp active mode)
     * @param url Server address
     * @param port Server port
     * @param cb Callback whether the connection to the server is successful
     */
    void connectToServer(const std::string &url, uint16_t port, const std::function<void(const toolkit::SockException &ex)> &cb);

    /**
     * Get the bound local port
     */
    uint16_t getPort();

    /**
     * Set RtpProcess onDetach event callback
     */
    void setOnDetach(RtpProcess::onDetachCB cb);

    /**
     * Update ssrc
     */
    void updateSSRC(uint32_t ssrc);

    uint32_t getSSRC() const;
    int getOnlyTrack() const { return _only_track; }
    TcpMode getTcpMode() const { return _tcp_mode; }
private:
    // tcp active mode connection server success callback
    void onConnect();

protected:
    toolkit::Socket::Ptr _rtp_socket;
    toolkit::UdpServer::Ptr _udp_server;
    toolkit::TcpServer::Ptr _tcp_server;
    std::shared_ptr<uint32_t> _ssrc;
    std::shared_ptr<RtcpHelper> _rtcp_helper;
    std::function<void()> _on_cleanup;

    int _only_track = 0;
    // Used for tcp active mode
    TcpMode _tcp_mode = NONE;
};

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
#endif //S3MEDIAKIT_RTPSERVER_H
