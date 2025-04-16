#ifndef HTTP_TSPLAYER_H
#define HTTP_TSPLAYER_H

#include "HttpTSPlayer.h"
#include "Player/PlayerBase.h"

namespace mediakit {

class TsPlayer : public HttpTSPlayer, public PlayerBase {
public:
    TsPlayer(const toolkit::EventPoller::Ptr &poller);

    /**
     * Start playing
     */
    void play(const std::string &url) override;

    /**
     * Stop playing
     */
    void teardown() override;

protected:
    void onResponseBody(const char *buf, size_t size) override;
    void onResponseCompleted(const toolkit::SockException &ex) override;

private:
    bool _play_result = true;
    bool _benchmark_mode = false;
};

} // namespace mediakit
#endif // HTTP_TSPLAYER_H
