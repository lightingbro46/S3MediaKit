#ifndef MEDIAKIT_BACKCHANNEL_WS_SESSION_H
#define MEDIAKIT_BACKCHANNEL_WS_SESSION_H

#include "Network/Session.h"
#include "Http/WebSocketSplitter.h"
#include "BackchannelMediaSource.h"
#include "BackchannelProtocol.h"

namespace mediakit {

class BackchannelWsSession : public toolkit::Session, public mediakit::WebSocketSplitter {
public:
    using Ptr = std::shared_ptr<BackchannelWsSession>;

    // `authDeviceId` is the device the upgrade request was authorized for (URL path): every
    // message must target it.
    BackchannelWsSession(const toolkit::Socket::Ptr& sock, const std::string& authDeviceId);
    ~BackchannelWsSession() override;

    void attachServer(const toolkit::Server& server) override;
    void onRecv(const toolkit::Buffer::Ptr& buffer) override;
    void onError(const toolkit::SockException& err) override;
    void onManager() override;

protected:
    void onWebSocketDecodeHeader(const mediakit::WebSocketHeader& header) override;
    void onWebSocketDecodePayload(const mediakit::WebSocketHeader& header, const uint8_t* ptr, size_t len, size_t recved) override;
    void onWebSocketDecodeComplete(const mediakit::WebSocketHeader& header) override;
    void onWebSocketEncodeData(toolkit::Buffer::Ptr buffer) override;

private:
    void dispatchMessage(const char* data, size_t len);
    void handleStart(const std::string& deviceId, const uint8_t* payload, size_t len);
    void handleAudioFrame(const std::string& deviceId, const uint8_t* payload, size_t len);
    void handleStop(const std::string& deviceId);
    void sendReply(BackchannelReplyType type, const std::string &reason);

private:
    const std::string _authDeviceId;
    std::string _deviceId;
    BackchannelEncoding _encoding = BackchannelEncoding::PCM16LE;
    std::string _payload_buf;
    toolkit::Ticker _alive_ticker;
};

} // namespace mediakit

#endif // MEDIAKIT_BACKCHANNEL_WS_SESSION_H