#ifndef S3MEDIAKIT_RTSPSPLITTER_H
#define S3MEDIAKIT_RTSPSPLITTER_H

#include "Common/Parser.h"
#include "Http/HttpRequestSplitter.h"

namespace mediakit{

class RtspSplitter : public HttpRequestSplitter{
public:
    /**
     * Whether to allow receiving rtp packets
     * @param enable
    */
    void enableRecvRtp(bool enable);
protected:
    /**
     * Callback for receiving a complete rtsp packet, including sdp and other content data
     * @param parser rtsp packet
     */
    virtual void onWholeRtspPacket(Parser &parser) = 0;

    /**
     * Callback for receiving rtp packets
     * @param data
     * @param len
     */
    virtual void onRtpPacket(const char *data,size_t len) = 0;

    /**
     * Get the Content length from the rtsp header
     * @param parser
     * @return
     */
    virtual ssize_t getContentLength(Parser &parser);

protected:
    const char *onSearchPacketTail(const char *data,size_t len) override ;
    const char *onSearchPacketTail_l(const char *data,size_t len) ;
    ssize_t onRecvHeader(const char *data,size_t len) override;
    void onRecvContent(const char *data,size_t len) override;

private:
    bool _enableRecvRtp = false;
    bool _isRtpPacket = false;
    Parser _parser;
};

}//namespace mediakit



#endif //S3MEDIAKIT_RTSPSPLITTER_H
