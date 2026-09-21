#include "BackchannelProtocol.h"
#include "Util/logger.h"

namespace mediakit {

namespace {
uint32_t readU32LE(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
}

bool BackchannelProtocol::parseMessage(const char *data, size_t len, BackchannelMessage &out) {
    if (!data || len < 2) return false;

    auto bytes = reinterpret_cast<const uint8_t *>(data);
    BackchannelMsgType type = static_cast<BackchannelMsgType>(bytes[0]);
    uint8_t idLen = bytes[1];

    if (type != BackchannelMsgType::AudioFrame &&
        type != BackchannelMsgType::Start &&
        type != BackchannelMsgType::Stop) {
        return false;
    }

    if (len < (size_t)(2 + idLen)) {
        WarnL << "Message is too short for the length declared";
        return false;
    }

    out.msg_type = type;
    out.device_id.assign(data + 2, idLen);
    out.header_size = (size_t)(2 + idLen);
    out.body = bytes + 2 + idLen;
    out.body_size = len - 2 - idLen;
    return true;
}

bool BackchannelProtocol::parseStartInfo(const uint8_t *data, size_t len, BackchannelStartInfo &out) {
    if (!data || len < 6) {
        WarnL << "Start message is missing sourceSampleRate, sourceChannels, or encoding";
        return false;
    }

    uint32_t sourceSampleRate = readU32LE(data);
    uint8_t sourceChannels = data[4];
    uint8_t encodingByte = data[5];

    if (sourceSampleRate == 0 || sourceChannels == 0 || sourceChannels > 2) {
        WarnL << "Invalid sourceSampleRate/sourceChannels (rate=" << sourceSampleRate
              << ", channels=" << static_cast<int>(sourceChannels) << ")";
        return false;
    }

    if (encodingByte != static_cast<uint8_t>(BackchannelEncoding::PCM16LE)) {
        WarnL << "Invalid encoding (currently only 0 = PCM16LE is supported): " << static_cast<int>(encodingByte);
        return false;
    }

    out.sample_rate = sourceSampleRate;
    out.channels = sourceChannels;
    out.encoding = static_cast<BackchannelEncoding>(encodingByte);
    return true;
}

std::string BackchannelProtocol::encodeReply(BackchannelReplyType type, const std::string &device_id, const std::string &reason) {
    size_t id_len = std::min<size_t>(device_id.size(), 255);

    std::string buf;
    buf.reserve(2 + id_len + reason.size());
    buf.push_back(static_cast<char>(static_cast<uint8_t>(type)));
    buf.push_back(static_cast<char>(static_cast<uint8_t>(id_len)));
    buf.append(device_id.data(), id_len);
    buf.append(reason);
    return buf;
}

} // namespace mediakit
