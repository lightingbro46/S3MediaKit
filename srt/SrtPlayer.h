#ifndef S3MEDIAKIT_SRTPLAYER_H
#define S3MEDIAKIT_SRTPLAYER_H

#include "Network/Socket.h"
#include "Player/PlayerBase.h"
#include "Poller/Timer.h"
#include "Util/TimeTicker.h"
#include "srt/SrtTransport.hpp"
#include "Http/HttpRequester.h"
#include <memory>
#include <string>
#include "SrtCaller.h"

namespace mediakit {


// Implemented the srt proxy stream pulling function
class SrtPlayer
    : public PlayerBase , public SrtCaller {
public:
    using Ptr = std::shared_ptr<SrtPlayer>;

    SrtPlayer(const toolkit::EventPoller::Ptr &poller);
    ~SrtPlayer() override;

    //// PlayerBase override////
    void play(const std::string &strUrl) override;
    void teardown() override;
    void pause(bool pause) override;
    void speed(float speed) override;
    size_t getRecvSpeed() override;
    size_t getRecvTotalBytes() override;

protected:

    //// SrtCaller override////
    void onHandShakeFinished() override;
    void onSRTData(SRT::DataPacket::Ptr pkt) override;
    void onResult(const toolkit::SockException &ex) override;

    bool isPlayer() override {return true;}

    uint16_t getLatency() override;
    float getTimeOutSec() override;
    std::string getPassphrase() override;

protected:
    //Is it a performance test mode?
    bool _benchmark_mode = false;

    //Timeout function implementation
    toolkit::Ticker _recv_ticker;
    std::shared_ptr<toolkit::Timer> _check_timer;
};

} /* namespace mediakit */
#endif /* S3MEDIAKIT_SRTPLAYER_H */
