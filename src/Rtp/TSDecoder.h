#ifndef S3MEDIAKIT_TSDECODER_H
#define S3MEDIAKIT_TSDECODER_H

#include "Util/logger.h"
#include "Http/HttpRequestSplitter.h"
#include "Decoder.h"

#define TS_PACKET_SIZE		188
#define TS_SYNC_BYTE        0x47

namespace mediakit {

// TS package splitter, used to split one ts package at a time
class TSSegment : public HttpRequestSplitter {
public:
    typedef std::function<void(const char *data,size_t len)> onSegment;
    TSSegment(size_t size = TS_PACKET_SIZE) : _size(size){}
    void setOnSegment(onSegment cb);
    static bool isTSPacket(const char *data, size_t len);

protected:
    ssize_t onRecvHeader(const char *data, size_t len) override ;
    const char *onSearchPacketTail(const char *data, size_t len) override ;

private:
    size_t _size;
    onSegment _onSegment;
};

#if defined(ENABLE_HLS)
// ts parser
class TSDecoder : public Decoder {
public:
    TSDecoder();
    ~TSDecoder();
    ssize_t input(const uint8_t* data, size_t bytes) override ;

private:
    TSSegment _ts_segment;
    struct ts_demuxer_t* _demuxer_ctx = nullptr;
};
#endif//defined(ENABLE_HLS)

}//namespace mediakit
#endif //S3MEDIAKIT_TSDECODER_H
