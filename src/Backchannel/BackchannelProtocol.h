#ifndef MEDIAKIT_BACKCHANNELPROTOCOL_H
#define MEDIAKIT_BACKCHANNELPROTOCOL_H

#include <cstdint>
#include <string>

namespace mediakit {

// Message format CLIENT -> SERVER:
// -- Start (messageType=2) --
//   Byte 0       : messageType (=2)
//   Byte 1       : deviceIdLen (N)
//   Byte 2..2+N  : deviceId (ASCII/UTF-8)
//   Byte 2+N     : sourceSampleRate (4 byte, LITTLE-ENDIAN uint32)
//   Byte 6+N     : sourceChannels (1 byte)
//   Byte 7+N     : encoding (1 byte: 0=PCM16LE, 1=Opus)
// -- AudioFrame (messageType=1) / Stop (messageType=3) --
//   Byte 0       : messageType
//   Byte 1       : deviceIdLen (N)
//   Byte 2..2+N  : deviceId
//   Byte 2+N..   : payload (when messageType = AudioFrame)
//
// Reply format SERVER -> CLIENT:
//   Byte 0       : replyType (1 = Started/OK, 2 = Busy, 3 = Error)
//   Byte 1       : deviceIdLen (N)
//   Byte 2..2+N  : deviceId (ASCII/UTF-8)
//   Byte 2+N..   : reason (UTF-8 string describing the reason; may be empty for OK)

enum class BackchannelMsgType : uint8_t { AudioFrame = 1, Start = 2, Stop = 3 };
enum class BackchannelReplyType : uint8_t { Started = 1, Busy = 2, Error = 3 };
enum class BackchannelEncoding : uint8_t { PCM16LE = 0, Opus = 1 };

struct BackchannelMessage {
    BackchannelMsgType msg_type;
    std::string device_id;
    const uint8_t* body;
    size_t body_size;
    size_t header_size;
};

struct BackchannelStartInfo {
    uint32_t sample_rate;
    uint8_t channels;
    BackchannelEncoding encoding;
};

class BackchannelProtocol {
public:

    static bool parseMessage(const char *data, size_t len, BackchannelMessage &out);

    static bool parseStartInfo(const uint8_t *data, size_t len, BackchannelStartInfo &out);

    static std::string encodeReply(BackchannelReplyType type, const std::string &device_id, const std::string &reason);
};

} // namespace mediakit
#endif // MEDIAKIT_BACKCHANNELPROTOCOL_H