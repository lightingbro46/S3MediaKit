#ifndef SRC_DEVICE_PUSHERPROXY_H
#define SRC_DEVICE_PUSHERPROXY_H

#include "Pusher/MediaPusher.h"
#include "Util/TimeTicker.h"

namespace mediakit {

class PusherProxy
    : public MediaPusher
    , public std::enable_shared_from_this<PusherProxy> {
public:
    using Ptr = std::shared_ptr<PusherProxy>;

    // If retry_count < 0, then retry playback indefinitely; otherwise, retry retry_count times
    // Default is to retry indefinitely. When creating this object, the external environment needs to ensure that MediaSource exists.
    PusherProxy(const MediaSource::Ptr &src, int retry_count = -1, const toolkit::EventPoller::Ptr &poller = nullptr);
    ~PusherProxy() override;

    /**
     * Set the push result callback, which is triggered only once; it is effective before publish is executed.
     * @param cb Callback object
     */
    void setPushCallbackOnce(const std::function<void(const toolkit::SockException &ex)> &cb);

    /**
     * Set the active close callback
     * @param cb Callback object
     */
    void setOnClose(const std::function<void(const toolkit::SockException &ex)> &cb);

    /**
     * Start pulling and playing the stream
     * @param dstUrl Target push stream address
     */
    void publish(const std::string &dstUrl) override;

    int getStatus();
    uint64_t getLiveSecs();
    uint64_t getRePublishCount();

private:
    // Repush logic function
    void rePublish(const std::string &dstUrl, int iFailedCnt);

private:
    int _retry_count;
    toolkit::Timer::Ptr _timer;
    toolkit::Ticker _live_ticker;
    // 0 indicates normal, 1 indicates that the push stream is being attempted
    std::atomic<int> _live_status;
    std::atomic<uint64_t> _live_secs;
    std::atomic<uint64_t> _republish_count;
    std::function<void(const toolkit::SockException &ex)> _on_close;
    std::function<void(const toolkit::SockException &ex)> _on_publish;
};

} /* namespace mediakit */

#endif // SRC_DEVICE_PUSHERPROXY_H
