#if defined(ENABLE_RTPPROXY)
#include <string.h>
#include "RtpSplitter.h"
namespace mediakit{

static const int  kEHOME_OFFSET = 256;

ssize_t RtpSplitter::onRecvHeader(const char *data,size_t len){
    // Ignore offset
    data += _offset;
    len -= _offset;

    if (_is_ehome && len > 12 && data[12] == '\r') {
        // This is ehome, remove the 12th byte
        memmove((char *) data + 1, data, 12);
        data += 1;
        len -= 1;
    }
    onRtpPacket(data, len);
    return 0;
}

static bool isEhome(const char *data, size_t len){
    if (len < 4) {
        return false;
    }
    if ((data[0] == 0x01) && (data[1] == 0x00) && (data[2] >= 0x01)) {
        return true;
    }
    return false;
}

const char *RtpSplitter::onSearchPacketTail(const char *data, size_t len) {
    if (len < 4) {
        // Not enough data
        return nullptr;
    }

    if (_check_ehome_count) {
        if (isEhome(data, len)) {
            // It is the ehome protocol
            if (len < kEHOME_OFFSET + 4) {
                // Not enough data
                return nullptr;
            }
            // Ignore the ehome private header, then it is an rtsp-style rtp, with 4 extra bytes,
            _offset = kEHOME_OFFSET + 4;
            _is_ehome = true;
            // Ignore the ehome private header
            return onSearchPacketTail_l(data + kEHOME_OFFSET + 2, len - kEHOME_OFFSET - 2);
        }
        _check_ehome_count--;
    }

    if ( _is_rtsp_interleaved ) {
        if (data[0] == '$') {
            // It may be a 4-byte rtp header
            _offset = 4;
            return onSearchPacketTail_l(data + 2, len - 2);
        }
        _is_rtsp_interleaved = false;
    }

    // A 2-byte rtp header
    _offset = 2;
    return onSearchPacketTail_l(data, len);
}

const char *RtpSplitter::onSearchPacketTail_l(const char *data, size_t len) {
    // This is an rtp packet
    uint16_t length = (((uint8_t *) data)[0] << 8) | ((uint8_t *) data)[1];
    if (len < (size_t)(length + 2)) {
        // Not enough data
        return nullptr;
    }
    // Return the end of the rtp packet
    return data + 2 + length;
}

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)