#ifndef SRC_RTMP_RtmpPlayer_H_
#define SRC_RTMP_RtmpPlayer_H_

#include <memory>
#include <string>
#include <functional>
#include "amf.h"
#include "Rtmp.h"
#include "RtmpProtocol.h"
#include "Player/PlayerBase.h"
#include "Util/TimeTicker.h"
#include "Network/Socket.h"
#include "Network/TcpClient.h"

namespace mediakit {

// Implemented the rtmp player protocol part functionality, and data receiving functionality
class RtmpPlayer : public PlayerBase, public toolkit::TcpClient, public RtmpProtocol {
public:
    using Ptr = std::shared_ptr<RtmpPlayer>;
    RtmpPlayer(const toolkit::EventPoller::Ptr &poller);
    ~RtmpPlayer() override;

    void play(const std::string &strUrl) override;
    void pause(bool bPause) override;
    void speed(float speed) override;
    void teardown() override;

    size_t getRecvSpeed() override;
    size_t getRecvTotalBytes() override;

protected:
    virtual bool onMetadata(const AMFValue &val) = 0;
    virtual void onRtmpPacket(RtmpPacket::Ptr chunk_data) = 0;
    uint32_t getProgressMilliSecond() const;
    void seekToMilliSecond(uint32_t ms);

protected:
    void onMediaData_l(RtmpPacket::Ptr chunk_data);
    // Trigger onPlayResult_l after getting the config frame (instead of receiving the play command reply), so all tracks are initialized at this time
    void onPlayResult_l(const toolkit::SockException &ex, bool handshake_done);

    //form Tcpclient
    void onRecv(const toolkit::Buffer::Ptr &buf) override;
    void onConnect(const toolkit::SockException &err) override;
    void onError(const toolkit::SockException &ex) override;
    //from RtmpProtocol
    void onRtmpChunk(RtmpPacket::Ptr chunk_data) override;
    void onStreamDry(uint32_t stream_index) override;
    void onSendRawData(toolkit::Buffer::Ptr buffer) override {
        send(std::move(buffer));
    }

    template<typename FUNC>
    void addOnResultCB(const FUNC &func) {
        _map_on_result.emplace(_send_req_id, func);
    }
    template<typename FUNC>
    void addOnStatusCB(const FUNC &func) {
        _deque_on_status.emplace_back(func);
    }

    void onCmd_result(AMFDecoder &dec);
    void onCmd_onStatus(AMFDecoder &dec);
    void onCmd_onMetaData(AMFDecoder &dec);

    void send_connect();
    void send_createStream();
    void send_play();
    void send_pause(bool pause);

private:
    std::string _app;
    std::string _stream_id;
    std::string _tc_url;

    bool _paused = false;
    bool _metadata_got = false;
    // Whether it is performance test mode
    bool _benchmark_mode = false;

    // Playback progress control
    uint32_t _seek_ms = 0;
    uint32_t _fist_stamp[2] = {0, 0};
    uint32_t _now_stamp[2] = {0, 0};
    toolkit::Ticker _now_stamp_ticker[2];
    std::deque<std::function<void(AMFValue &dec)> > _deque_on_status;
    std::unordered_map<int, std::function<void(AMFDecoder &dec)> > _map_on_result;

    // Rtmp receive timeout timer
    toolkit::Ticker _rtmp_recv_ticker;
    // Heartbeat send timer
    std::shared_ptr<toolkit::Timer> _beat_timer;
    // Playback timeout timer
    std::shared_ptr<toolkit::Timer> _play_timer;
    // Rtmp receive timeout timer
    std::shared_ptr<toolkit::Timer> _rtmp_recv_timer;
};

} /* namespace mediakit */
#endif /* SRC_RTMP_RtmpPlayer_H_ */
