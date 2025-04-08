#ifndef SRC_RTMP_RTMPSESSION_H_
#define SRC_RTMP_RTMPSESSION_H_

#include <unordered_map>
#include "amf.h"
#include "Rtmp.h"
#include "utils.h"
#include "RtmpProtocol.h"
#include "RtmpMediaSourceImp.h"
#include "Util/TimeTicker.h"
#include "Network/Session.h"

namespace mediakit {

class RtmpSession : public toolkit::Session, public RtmpProtocol, public MediaSourceEvent {
public:
    using Ptr = std::shared_ptr<RtmpSession>;

    RtmpSession(const toolkit::Socket::Ptr &sock);

    void onRecv(const toolkit::Buffer::Ptr &buf) override;
    void onError(const toolkit::SockException &err) override;
    void onManager() override;

private:
    void onProcessCmd(AMFDecoder &dec);
    void onCmd_connect(AMFDecoder &dec);
    void onCmd_createStream(AMFDecoder &dec);

    void onCmd_publish(AMFDecoder &dec);
    void onCmd_deleteStream(AMFDecoder &dec);

    void onCmd_play(AMFDecoder &dec);
    void onCmd_play2(AMFDecoder &dec);
    void doPlay(AMFDecoder &dec);
    void doPlayResponse(const std::string &err,const std::function<void(bool)> &cb);
    void sendPlayResponse(const std::string &err,const RtmpMediaSource::Ptr &src);

    void onCmd_seek(AMFDecoder &dec);
    void onCmd_pause(AMFDecoder &dec);
    void onCmd_playCtrl(AMFDecoder &dec);
    void setMetaData(AMFDecoder &dec);

    void onSendMedia(const RtmpPacket::Ptr &pkt);
    void onSendRawData(toolkit::Buffer::Ptr buffer) override{
        _total_bytes += buffer->size();
        send(std::move(buffer));
    }
    void onRtmpChunk(RtmpPacket::Ptr chunk_data) override;

    template<typename first, typename second>
    inline void sendReply(const char *str, const first &reply, const second &status) {
        AMFEncoder invoke;
        invoke << str << _recv_req_id << reply << status;
        sendResponse(MSG_CMD, invoke.data());
    }

    ///////MediaSourceEvent override///////
    // Close
    bool close(MediaSource &sender) override;
    // Total number of plays
    int totalReaderCount(MediaSource &sender) override;
    // Get media source type
    MediaOriginType getOriginType(MediaSource &sender) const override;
    // Get media source url or file path
    std::string getOriginUrl(MediaSource &sender) const override;
    // Get media source client related information
    std::shared_ptr<SockInfo> getOriginSock(MediaSource &sender) const override;
    // Due to support for discontinuous pushing, there may be changes in OwnerPoller
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;

    void setSocketFlags();
    std::string getStreamId(const std::string &str);
    void dumpMetadata(const AMFValue &metadata);
    void sendStatus(const std::initializer_list<std::string> &key_value);

private:
    bool _set_meta_data = false;
    double _recv_req_id = 0;
    // Discontinuous pushing delay
    uint32_t _continue_push_ms = 0;
    // Total traffic consumed
    uint64_t _total_bytes = 0;
    // Data reception timeout timer
    toolkit::Ticker _ticker;
    MediaInfo _media_info;
    std::weak_ptr<RtmpMediaSource> _play_src;
    AMFValue _push_metadata;
    std::map<uint8_t, RtmpPacket::Ptr> _push_config_packets;
    RtmpMediaSourceImp::Ptr _push_src;
    std::shared_ptr<void> _push_src_ownership;
    RtmpMediaSource::RingType::RingReader::Ptr _ring_reader;
};

/**
 * Supports ssl encrypted rtmp server
 */
using RtmpSessionWithSSL = toolkit::SessionWithSSL<RtmpSession>;

} /* namespace mediakit */
#endif /* SRC_RTMP_RTMPSESSION_H_ */
