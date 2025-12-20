#ifndef HTTP_TSPLAYERIMP_H
#define HTTP_TSPLAYERIMP_H

#include <unordered_set>
#include "TsPlayer.h"

namespace mediakit {

class TsPlayerImp : public PlayerImp<TsPlayer, PlayerBase>, private TrackListener {
public:
    using Ptr = std::shared_ptr<TsPlayerImp>;

    TsPlayerImp(const toolkit::EventPoller::Ptr &poller = nullptr);
    size_t getRecvSpeed() override;
    size_t getRecvTotalBytes() override;

private:
    //// TsPlayer override////
    void onResponseBody(const char *buf, size_t size) override;

private:
    //// PlayerBase override////
    void onPlayResult(const toolkit::SockException &ex) override;
    std::vector<Track::Ptr> getTracks(bool ready = true) const override;
    void onShutdown(const toolkit::SockException &ex) override;

private:
    //// TrackListener override////
    bool addTrack(const Track::Ptr &track) override { return true; };
    void addTrackCompleted() override;

private:
    DecoderImp::Ptr _decoder;
    MediaSinkInterface::Ptr _demuxer;
};

}//namespace mediakit
#endif //HTTP_TSPLAYERIMP_H
