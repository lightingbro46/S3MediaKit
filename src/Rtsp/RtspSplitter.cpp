#include <cstdlib>
#include "RtspSplitter.h"
#include "Util/util.h"
#include "Util/logger.h"
#include "Common/macros.h"
#include "Rtsp/RtpReceiver.h"

using namespace std;
using namespace toolkit;

namespace mediakit{

const char *RtspSplitter::onSearchPacketTail(const char *data, size_t len) {
    auto ret = onSearchPacketTail_l(data, len);
    if(ret){
        return ret;
    }

    if (len > 256 * 1024) {
        // rtp is greater than 256KB
        ret = (char *) memchr(data, '$', len);
        if (!ret) {
            WarnL << "rtp cache overflow:" << hexdump(data, 1024);
            reset();
        }
    }
    return ret;
}

const char *RtspSplitter::onSearchPacketTail_l(const char *data, size_t len) {
    if(!_enableRecvRtp || data[0] != '$'){
        // This is an rtsp packet
        _isRtpPacket = false;
        return HttpRequestSplitter::onSearchPacketTail(data, len);
    }
    // This is an rtp packet
    if(len < 4){
        // Not enough data
        return nullptr;
    }
    uint16_t length = (((uint8_t *)data)[2] << 8) | ((uint8_t *)data)[3];
    if(len < (size_t)(length + 4)){
        // Not enough data
        return nullptr;
    }
    // Return the end of the rtp packet
    _isRtpPacket = true;
    return data + 4 + length;
}

ssize_t RtspSplitter::onRecvHeader(const char *data, size_t len) {
    if (_isRtpPacket) {
        try {
            onRtpPacket(data, len);
        } catch (RtpTrack::BadRtpException &ex) {
            WarnL << ex.what();
        }
        return 0;
    }
    if (len == 4 && !memcmp(data, "\r\n\r\n", 4)) {
        return 0;
    }
    try {
        _parser.parse(data, len);
    } catch (toolkit::AssertFailedException &ex){
        if (!_enableRecvRtp) {
            // Still in handshake, interrupt handshake directly
            throw;
        }
        // Handshake has ended, if rtsp server has a send buffer overflow bug, then rtsp signaling may be mixed with rtp
        // In this case, rtsp signaling parsing exception does not interrupt the connection, just discard this packet
        WarnL << ex.what();
        return 0;
    }
    auto ret = getContentLength(_parser);
    if (ret == 0) {
        onWholeRtspPacket(_parser);
        _parser.clear();
    }
    return ret;
}

void RtspSplitter::onRecvContent(const char *data, size_t len) {
    _parser.setContent(string(data,len));
    onWholeRtspPacket(_parser);
    _parser.clear();
}

void RtspSplitter::enableRecvRtp(bool enable) {
    _enableRecvRtp = enable;
}

ssize_t RtspSplitter::getContentLength(Parser &parser) {
    return atoi(parser["Content-Length"].data());
}


}//namespace mediakit



