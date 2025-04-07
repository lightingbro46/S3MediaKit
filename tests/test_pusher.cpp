#include <signal.h>
#include <iostream>
#include "Util/logger.h"
#include "Util/NoticeCenter.h"
#include "Poller/EventPoller.h"
#include "Player/PlayerProxy.h"
#include "Rtmp/RtmpPusher.h"
#include "Common/config.h"
#include "Pusher/MediaPusher.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

// Streamer, keep a strong reference
MediaPusher::Ptr pusher;
Timer::Ptr g_timer;

// Declare function
void rePushDelay(const EventPoller::Ptr &poller,const string &schema,const string &vhost,const string &app, const string &stream, const string &url);

// Create a streamer and start streaming
void createPusher(const EventPoller::Ptr &poller, const string &schema,const string &vhost,const string &app, const string &stream, const string &url) {
    // Create a streamer and bind a MediaSource
    pusher.reset(new MediaPusher(schema,vhost, app, stream,poller));
    // You can specify the RTSP streaming method, supporting both TCP and UDP methods, defaulting to TCP
//    (*pusher)[Client::kRtpType] = Rtsp::RTP_UDP;
    // Set the streaming interruption handling logic
    pusher->setOnShutdown([poller,schema,vhost, app, stream, url](const SockException &ex) {
        WarnL << "Server connection is closed:" << ex.getErrCode() << " " << ex.what();
        // Retry
        rePushDelay(poller,schema,vhost,app, stream, url);
    });
    // Set the publishing result handling logic
    pusher->setOnPublished([poller,schema,vhost, app, stream, url](const SockException &ex) {
        if (ex) {
            WarnL << "Publish fail:" << ex.getErrCode() << " " << ex.what();
            // If publishing fails, retry
            rePushDelay(poller,schema,vhost,app, stream, url);
        } else {
            InfoL << "Publish success,Please play with player:" << url;
        }
    });
    pusher->publish(url);
}

// If streaming fails or is disconnected, retry streaming after a 2-second delay
void rePushDelay(const EventPoller::Ptr &poller,const string &schema,const string &vhost,const string &app, const string &stream, const string &url) {
    g_timer = std::make_shared<Timer>(2.0f,[poller,schema,vhost,app, stream, url]() {
        InfoL << "Re-Publishing...";
        // Re-stream
        createPusher(poller,schema,vhost,app, stream, url);
        // This task is not repeated
        return false;
    }, poller);
}

// This is where the main function is actually executed, you can change the function name (domain) to main, and then you can enter a custom URL
int domain(const string &playUrl, const string &pushUrl) {
    // Set the log
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());
    auto poller = EventPollerPool::Instance().getPoller();

    // Pull a stream and generate an RtmpMediaSource, the source name is "app/stream"
    // You can also generate RtmpMediaSource in other ways, such as an MP4 file (please refer to the test_rtmpPusherMp4.cpp code)
    MediaInfo info(pushUrl);

    ProtocolOption option;
    option.enable_hls = false;
    option.enable_mp4 = false;
    auto tuple = MediaTuple{DEFAULT_VHOST, "app", "stream", ""};
    PlayerProxy::Ptr player(new PlayerProxy(tuple, option, -1, poller));
    // You can specify the RTSP streaming method, supporting both TCP and UDP methods, defaulting to TCP
//    (*player)[Client::kRtpType] = Rtsp::RTP_UDP;
    player->play(playUrl.data());

    // Listen for RtmpMediaSource registration events, triggered after PlayerProxy playback is successful
    NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaChanged,
                                         [pushUrl,poller](BroadcastMediaChangedArgs) {
                                             // The media source "app/stream" has been registered, at this point you can create a new RtmpPusher object and bind it to the media source
                                             if (bRegist && pushUrl.find(sender.getSchema()) == 0) {
                                                 auto tuple = sender.getMediaTuple();
                                                 createPusher(poller, sender.getSchema(), tuple.vhost, tuple.app, tuple.stream, pushUrl);
                                             }
                                         });

    // Set the exit signal processing function
    static semaphore sem;
    signal(SIGINT, [](int) { sem.post(); });// Set the exit signal
    sem.wait();
    pusher.reset();
    g_timer.reset();
    return 0;
}


int main(int argc, char *argv[]) {
    return domain("rtmp://live.hkstv.hk.lxdns.com/live/hks1", "rtsp://127.0.0.1/live/rtsp_push");
}






