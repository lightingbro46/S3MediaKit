#ifndef S3MEDIAKIT_HTTPCHUNKEDSPLITTER_H
#define S3MEDIAKIT_HTTPCHUNKEDSPLITTER_H

#include <functional>
#include "HttpRequestSplitter.h"

namespace mediakit{

class HttpChunkedSplitter : public HttpRequestSplitter {
public:
    /**
     * When len == 0, it represents the end.
     */
   using onChunkData = std::function<void(const char *data, size_t len)>;

    HttpChunkedSplitter(const onChunkData &cb) { _onChunkData = cb; };
    ~HttpChunkedSplitter() override { _onChunkData = nullptr; };

protected:
    ssize_t onRecvHeader(const char *data,size_t len) override;
    void onRecvContent(const char *data,size_t len) override;
    const char *onSearchPacketTail(const char *data,size_t len) override;

protected:
    virtual void onRecvChunk(const char *data,size_t len){
        if(_onChunkData){
            _onChunkData(data,len);
        }
    };

private:
    onChunkData _onChunkData;
};

}//namespace mediakit
#endif //S3MEDIAKIT_HTTPCHUNKEDSPLITTER_H
