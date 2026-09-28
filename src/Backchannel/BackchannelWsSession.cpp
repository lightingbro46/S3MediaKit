#include "BackchannelWsSession.h"
#include "Backchannel/BackchannelManager.h"
#include "Util/logger.h"
#include "Common/config.h"

using namespace toolkit;

namespace mediakit {

BackchannelWsSession::BackchannelWsSession(const Socket::Ptr& sock, const std::string& authDeviceId)
    : Session(sock), _authDeviceId(authDeviceId) {
    TraceL << "Created backchannel session: " << getIdentifier();
}

BackchannelWsSession::~BackchannelWsSession() {
    if (!_deviceId.empty()) {
        BackchannelManager::Instance().stopSession(_deviceId);
    }
    TraceL << "Destroyed backchannel session: " << getIdentifier();
}

void BackchannelWsSession::attachServer(const Server& server) {
    TraceL << getIdentifier();
}

void BackchannelWsSession::onError(const SockException& err) {
    WarnL << "Backchannel session error: " << err.what();
}

void BackchannelWsSession::onManager() {
    GET_CONFIG(size_t, keep_alive_sec, Http::kKeepAliveSecond);
    if (_alive_ticker.elapsedTime() > keep_alive_sec * 1000) {
        if (!_deviceId.empty()) {
            BackchannelManager::Instance().stopSession(_deviceId);
            _deviceId.clear();
        }
        shutdown(SockException(Err_timeout, "backchannel idle timeout"));
    }
}

void BackchannelWsSession::onRecv(const Buffer::Ptr& buffer) {
    _alive_ticker.resetTime();
    WebSocketSplitter::decode(reinterpret_cast<uint8_t*>(buffer->data()), buffer->size());
}

void BackchannelWsSession::onWebSocketDecodeHeader(const WebSocketHeader& header) {
    _payload_buf.clear();
    _payload_buf.reserve(header._payload_len);
}

void BackchannelWsSession::onWebSocketDecodePayload(const WebSocketHeader& header, const uint8_t* ptr, size_t len, size_t recved) {
    _payload_buf.append(reinterpret_cast<const char*>(ptr), len);
}

void BackchannelWsSession::onWebSocketDecodeComplete(const WebSocketHeader& header) {
    if (header._opcode == WebSocketHeader::BINARY) {
        dispatchMessage(_payload_buf.data(), _payload_buf.size());
    }
}

void BackchannelWsSession::onWebSocketEncodeData(Buffer::Ptr buffer) {
    send(std::move(buffer));
}

void BackchannelWsSession::dispatchMessage(const char* data, size_t len) {
    BackchannelMessage msg;
    if (!BackchannelProtocol::parseMessage(data, len, msg)) {
        sendReply(BackchannelReplyType::Error, "malformed message");
        return;
    }

    // Authorization covered the device in the URL only; a message naming another device would
    // start, feed or stop that device's session without the user's permission on it.
    if (msg.device_id != _authDeviceId) {
        WarnL << "Backchannel message for device " << msg.device_id << " rejected, session is authorized for " << _authDeviceId;
        sendReply(BackchannelReplyType::Error, "device mismatch");
        return;
    }

    switch (msg.msg_type) {
        case BackchannelMsgType::Start:         handleStart(msg.device_id, msg.body, msg.body_size); break;
        case BackchannelMsgType::AudioFrame:    handleAudioFrame(msg.device_id, msg.body, msg.body_size); break;
        case BackchannelMsgType::Stop:          handleStop(msg.device_id); break;
        default: sendReply(BackchannelReplyType::Error, "unknown message type"); break;
    }
}

void BackchannelWsSession::handleStart(const std::string& deviceId, const uint8_t* payload, size_t len) {
    BackchannelStartInfo info;
    if (!BackchannelProtocol::parseStartInfo(payload, len, info)) {
        sendReply(BackchannelReplyType::Error, "malformed start message");
        return;
    }

    auto result = BackchannelManager::Instance().startLiveSession(deviceId, info.sample_rate, info.channels);
    switch (result) {
        case StartResult::DeviceBusy:
            WarnL << "Device is controlled by other user";
            sendReply(BackchannelReplyType::Busy, "Device is controlled by other user");
            return;
        case StartResult::DeviceNotRegistered:
            WarnL << "Failed to start live session for device: " << deviceId << " (device not registered)";
            sendReply(BackchannelReplyType::Error, "Device not registered");
            return;
        case StartResult::Started:
            _deviceId = deviceId;
            _encoding = info.encoding;
            InfoL << "Started live streaming to device: " << _deviceId
                    << " (sampleRate=" << info.sample_rate
                    << ", channels=" << static_cast<int>(info.channels)
                    << ", encoding=" << (_encoding == BackchannelEncoding::Opus ? "Opus" : "PCM16LE") << ")";
            sendReply(BackchannelReplyType::Started, "");
            return;
    }
}

void BackchannelWsSession::handleAudioFrame(const std::string& deviceId, const uint8_t* payload, size_t len) {
    if (!payload || len == 0) return;

    if (_encoding == BackchannelEncoding::Opus) return;

    const int16_t* pcmPtr = reinterpret_cast<const int16_t*>(payload);
    size_t pcmSamples = len / sizeof(int16_t);
    if (pcmSamples == 0) return;

    if (!BackchannelManager::Instance().pushAudioFrame(deviceId, pcmPtr, pcmSamples)) {
        TraceL << "Received an audio frame, but no active live session exists for device: " << deviceId;
    }

    // TODO: implement Opus
}

void BackchannelWsSession::handleStop(const std::string& deviceId) {
    BackchannelManager::Instance().stopSession(deviceId);
    if (_deviceId == deviceId) _deviceId.clear();
    InfoL << "Stopped live streaming to device: " << deviceId;
}

void BackchannelWsSession::sendReply(BackchannelReplyType type, const std::string &reason) {
    if (_deviceId.size() > 255) {
        WarnL << "Device ID is too long. Reply will not be sent";
        return;
    }
    auto payload = BackchannelProtocol::encodeReply(type, _deviceId, reason);
    auto buf = BufferRaw::create();
    buf->assign(payload.data(), payload.size());

    WebSocketHeader header;
    header._fin = true;
    header._reserved = 0;
    header._opcode = WebSocketHeader::BINARY;
    header._mask_flag = false;
    WebSocketSplitter::encode(header, buf);
}

} // namespace mediakit