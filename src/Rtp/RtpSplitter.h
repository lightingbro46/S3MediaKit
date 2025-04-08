#ifndef S3MEDIAKIT_RTPSPLITTER_H
#define S3MEDIAKIT_RTPSPLITTER_H

#if defined(ENABLE_RTPPROXY)
#include "Http/HttpRequestSplitter.h"

namespace mediakit{

class RtpSplitter : public HttpRequestSplitter{
protected:
    /**
     * RTP packet received callback
     * @param data RTP packet data pointer
     * @param len RTP packet data length
     */
    virtual void onRtpPacket(const char *data, size_t len) = 0;

protected:
    ssize_t onRecvHeader(const char *data, size_t len) override;
    const char *onSearchPacketTail(const char *data, size_t len) override;
    const char *onSearchPacketTail_l(const char *data, size_t len);

private:
    bool _is_ehome = false;
    int _check_ehome_count = 3;
    bool _is_rtsp_interleaved = true;
    size_t _offset = 0;
};

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
#endif //S3MEDIAKIT_RTPSPLITTER_H
