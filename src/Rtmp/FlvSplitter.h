#ifndef S3MEDIAKIT_FLVSPLITTER_H
#define S3MEDIAKIT_FLVSPLITTER_H

#include "Rtmp.h"
#include "Http/HttpRequestSplitter.h"
#include "RtmpPlayerImp.h"

namespace mediakit {

class FlvSplitter : public HttpRequestSplitter {
protected:
    void onRecvContent(const char *data,size_t len) override;
    ssize_t onRecvHeader(const char *data,size_t len) override;
    const char *onSearchPacketTail(const char *data, size_t len) override;

protected:
    virtual void onRecvFlvHeader(const FLVHeader &header) {};
    virtual bool onRecvMetadata(const AMFValue &metadata) = 0;
    virtual void onRecvRtmpPacket(RtmpPacket::Ptr packet) = 0;

private:
    bool _flv_started = false;
    uint8_t _type;
    uint32_t _time_stamp;
};

}//namespace mediakit
#endif //S3MEDIAKIT_FLVSPLITTER_H
