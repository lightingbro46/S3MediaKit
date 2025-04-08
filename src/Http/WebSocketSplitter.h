#ifndef S3MEDIAKIT_WEBSOCKETSPLITTER_H
#define S3MEDIAKIT_WEBSOCKETSPLITTER_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "Network/Buffer.h"

// websocket combined package size must not exceed 4MB (to prevent memory explosion)
#define MAX_WS_PACKET (4 * 1024 * 1024)

namespace mediakit {

class WebSocketHeader {
public:
    using Ptr = std::shared_ptr<WebSocketHeader>;
    typedef enum {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        RSV3 = 0x3,
        RSV4 = 0x4,
        RSV5 = 0x5,
        RSV6 = 0x6,
        RSV7 = 0x7,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA,
        CONTROL_RSVB = 0xB,
        CONTROL_RSVC = 0xC,
        CONTROL_RSVD = 0xD,
        CONTROL_RSVE = 0xE,
        CONTROL_RSVF = 0xF
    } Type;
public:

    WebSocketHeader() : _mask(4){
        // Get the memory address of the internal buffer of _mask, the memory is allocated by malloc, and the address is random
        uint64_t ptr = (uint64_t)(&_mask[0]);
        // Set the mask random number according to the memory address
        _mask.assign((uint8_t*)(&ptr), (uint8_t*)(&ptr) + 4);
    }

    virtual ~WebSocketHeader() = default;

public:
    bool _fin;
    uint8_t _reserved;
    Type _opcode;
    bool _mask_flag;
    size_t _payload_len;
    std::vector<uint8_t > _mask;
};

// String type cache received by the websocket protocol, the way the user protocol layer obtains this data transmission
class WebSocketBuffer : public toolkit::BufferString {
public:
    using Ptr = std::shared_ptr<WebSocketBuffer>;

    template<typename ...ARGS>
    WebSocketBuffer(WebSocketHeader::Type headType, bool fin, ARGS &&...args)
            :  toolkit::BufferString(std::forward<ARGS>(args)...), _fin(fin), _head_type(headType){}

    WebSocketHeader::Type headType() const { return _head_type; }

    bool isFinished() const { return _fin; };

private:
    bool _fin;
    WebSocketHeader::Type _head_type;
};

class WebSocketSplitter : public WebSocketHeader{
public:
    /**
     * Input data to unpack webSocket data and handle sticky packet problems
     * May trigger onWebSocketDecodeHeader and onWebSocketDecodePayload callbacks
     * @param data Data to be unpacked, may be incomplete packets or multiple packets
     * @param len Data length
     */
    void decode(uint8_t *data, size_t len);

    /**
     * Encode a data packet
     * Will trigger 2 onWebSocketEncodeData callbacks
     * @param header Data header
     * @param buffer Payload data
     */
    void encode(const WebSocketHeader &header,const toolkit::Buffer::Ptr &buffer);

protected:
    /**
     * Receive a webSocket data packet header, and will continue to trigger onWebSocketDecodePayload callback
     * @param header Data packet header
     */
    virtual void onWebSocketDecodeHeader(const WebSocketHeader &header) {};

    /**
     * Receive webSocket data packet payload
     * @param header Data packet header
     * @param ptr Payload data pointer
     * @param len Payload data length
     * @param recved Received data length (including the length of this data), equals header._payload_len when the reception is complete
     */
    virtual void onWebSocketDecodePayload(const WebSocketHeader &header, const uint8_t *ptr, size_t len, size_t recved) {};

    /**
     * Callback after receiving a complete webSocket data packet
     * @param header Data packet header
     */
    virtual void onWebSocketDecodeComplete(const WebSocketHeader &header) {};

    /**
     * websocket data encoding callback
     * @param ptr Data pointer
     * @param len Data pointer length
     */
    virtual void onWebSocketEncodeData(toolkit::Buffer::Ptr buffer){};

private:
    void onPayloadData(uint8_t *data, size_t len);

private:
    bool _got_header = false;
    int _mask_offset = 0;
    size_t _payload_offset = 0;
    std::string _remain_data;
};

} /* namespace mediakit */


#endif //S3MEDIAKIT_WEBSOCKETSPLITTER_H
