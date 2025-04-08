#ifndef S3MEDIAKIT_SRTPUSHER_H
#define S3MEDIAKIT_SRTPUSHER_H

#include "Network/Socket.h"
#include "Pusher/PusherBase.h"
#include "Poller/Timer.h"
#include "Util/TimeTicker.h"
#include "srt/SrtTransport.hpp"
#include "Http/HttpRequester.h"
#include <memory>
#include <string>
#include "SrtCaller.h"

namespace mediakit {

// Implemented the srt proxy streaming function
class SrtPusher
    : public PusherBase , public SrtCaller {
public:
    using Ptr = std::shared_ptr<SrtPusher>;

    SrtPusher(const toolkit::EventPoller::Ptr &poller,const TSMediaSource::Ptr &src);
    ~SrtPusher() override;

    //// PusherBase override////
    void publish(const std::string &url) override;
    void teardown() override;

    void doPublish();
protected:

    //// SrtCaller override////
    void onHandShakeFinished() override;
    void onResult(const toolkit::SockException &ex) override;

    bool isPlayer() override {return false;}
    uint16_t getLatency() override;
    float getTimeOutSec() override;
    std::string getPassphrase() override;

protected:
    std::weak_ptr<TSMediaSource> _push_src;
    TSMediaSource::RingType::RingReader::Ptr _ts_reader;
};

using SrtPusherImp = PusherImp<SrtPusher, PusherBase>;

} /* namespace mediakit */
#endif /* S3MEDIAKIT_SRTPUSHER_H */
