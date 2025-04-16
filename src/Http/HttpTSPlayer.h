#ifndef HTTP_HTTPTSPLAYER_H
#define HTTP_HTTPTSPLAYER_H

#include "Http/HttpDownloader.h"
#include "Player/MediaPlayer.h"
#include "Rtp/TSDecoder.h"

namespace mediakit {

// http-ts broadcaster, ts demultiplexing not implemented
class HttpTSPlayer : public HttpClientImp {
public:
    using Ptr = std::shared_ptr<HttpTSPlayer>;
    using onComplete = std::function<void(const toolkit::SockException &)>;

    HttpTSPlayer(const toolkit::EventPoller::Ptr &poller = nullptr);

    /**
     * Set the callback for download completion or abnormal disconnection
     */
    void setOnComplete(onComplete cb);

    /**
     * Set the callback for receiving ts packets
     */
    void setOnPacket(TSSegment::onSegment cb);

protected:
    ///HttpClient override///
    void onResponseHeader(const std::string &status, const HttpHeader &header) override;
    void onResponseBody(const char *buf, size_t size) override;
    void onResponseCompleted(const toolkit::SockException &ex) override;

private:
    void emitOnComplete(const toolkit::SockException &ex);

private:
    onComplete _on_complete;
    TSSegment::onSegment _on_segment;
};

}//namespace mediakit
#endif //HTTP_HTTPTSPLAYER_H
