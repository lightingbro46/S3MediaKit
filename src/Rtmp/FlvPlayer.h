#ifndef S3MEDIAKIT_FLVPLAYER_H
#define S3MEDIAKIT_FLVPLAYER_H

#include "FlvSplitter.h"
#include "Http/HttpClientImp.h"
#include "Player/PlayerBase.h"

namespace mediakit {

class FlvPlayer : public PlayerBase, public HttpClientImp, private FlvSplitter {
public:
    FlvPlayer(const toolkit::EventPoller::Ptr &poller);

    void play(const std::string &url) override;
    void teardown() override;

protected:
    void onResponseHeader(const std::string &status, const HttpHeader &header) override;
    void onResponseCompleted(const toolkit::SockException &ex) override;
    void onResponseBody(const char *buf, size_t size) override;

protected:
    virtual void onRtmpPacket(RtmpPacket::Ptr packet) = 0;
    virtual bool onMetadata(const AMFValue &metadata) = 0;

private:
    bool onRecvMetadata(const AMFValue &metadata) override;
    void onRecvRtmpPacket(RtmpPacket::Ptr packet) override;

private:
    bool _play_result = false;
    bool _benchmark_mode = false;
};

using FlvPlayerImp = FlvPlayerBase<FlvPlayer>;

}//namespace mediakit
#endif //S3MEDIAKIT_FLVPLAYER_H
