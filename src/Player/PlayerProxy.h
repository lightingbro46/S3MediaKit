#ifndef SRC_DEVICE_PLAYERPROXY_H_
#define SRC_DEVICE_PLAYERPROXY_H_

#include "Common/MultiMediaSourceMuxer.h"
#include "Player/MediaPlayer.h"
#include "Util/TimeTicker.h"
#include <memory>

namespace mediakit {

struct StreamInfo
{
    TrackType codec_type;
    std::string codec_name;
    int bitrate;
    int audio_sample_rate;
    int audio_sample_bit;
    int audio_channel;
    int video_width;
    int video_height;
    float video_fps;

    StreamInfo()
    {
        codec_type = TrackInvalid;
        codec_name = "none";
        bitrate = -1;
        audio_sample_rate = -1;
        audio_channel = -1;
        audio_sample_bit = -1;
        video_height = -1;
        video_width = -1;
        video_fps = -1.0;
    }
};

struct TranslationInfo
{
    std::vector<StreamInfo> stream_info;
    int byte_speed;
    uint64_t start_time_stamp;

    TranslationInfo()
    {
        byte_speed = -1;
        start_time_stamp = 0;
    }
};

class PlayerProxy
    : public MediaPlayer
    , public MediaSourceEvent
    , public std::enable_shared_from_this<PlayerProxy> {
public:
    using Ptr = std::shared_ptr<PlayerProxy>;

    // If retry_count < 0, then retry playing indefinitely; otherwise, retry retry_count times
    // Default to retrying indefinitely
    PlayerProxy(const MediaTuple &tuple, const ProtocolOption &option, int retry_count = -1,
        const toolkit::EventPoller::Ptr &poller = nullptr, 
        int reconnect_delay_min = 2, int reconnect_delay_max = 60, int reconnect_delay_step = 3);

    ~PlayerProxy() override;

    /**
     * Set a callback for the play result, triggered only once; effective before play execution
     * @param cb Callback object
     */
    void setPlayCallbackOnce(std::function<void(const toolkit::SockException &ex)> cb);

    /**
     * Set a callback for active closure
     * @param cb Callback object
     */
    void setOnClose(std::function<void(const toolkit::SockException &ex)> cb);

    /**
     * Set a callback for failed server connection
     * @param cb Callback object
    */
    void setOnDisconnect(std::function<void(const toolkit::SockException &ex)> cb);

    /**
     * Set a callback for a successful connection to the server
     * @param cb Callback object
    */
    void setOnConnect(std::function<void(const TranslationInfo&)> cb);

    /**
     * Start streaming playback
     * @param strUrl
     */
    void play(const std::string &strUrl) override;

    /**
     * Get the total number of viewers
     */
    int totalReaderCount();

    int getStatus();
    uint64_t getLiveSecs();
    uint64_t getRePullCount();

    // Using this only makes sense after a successful connection to the server
    TranslationInfo getTranslationInfo();

    const std::string& getUrl() const { return _pull_url; }
    const MediaTuple& getMediaTuple() const { return _tuple; }
    const ProtocolOption& getOption() const { return _option; }
    void setReplayRecorderTimeFile(const time_t &recoder_time_file) { _replay_recoder_time_file = recoder_time_file; }
    void setOnReplayClose(std::function<void(const std::string &proxyKey, const float &progress)> cb);
    void setOnReplayRetry(std::function<bool(const std::string &proxyKey, const float &progress, std::string &newUrl)> cb);

private:
    // MediaSourceEvent override
    bool close(MediaSource &sender) override;
    int totalReaderCount(MediaSource &sender) override;
    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    std::shared_ptr<toolkit::SockInfo> getOriginSock(MediaSource &sender) const override;
    float getLossRate(MediaSource &sender, TrackType type) override;
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;

    void rePlay(const std::string &strUrl, int iFailedCnt);
    void onPlaySuccess();
    void setDirectProxy();
    void setTranslationInfo();

private:
    int _retry_count;
    int _reconnect_delay_min;
    int _reconnect_delay_max;
    int _reconnect_delay_step;
    MediaTuple _tuple;
    ProtocolOption _option;
    std::string _pull_url;
    toolkit::Timer::Ptr _timer;
    std::function<void(const toolkit::SockException &ex)> _on_disconnect;
    std::function<void(const TranslationInfo &info)> _on_connect;
    std::function<void(const toolkit::SockException &ex)> _on_close;
    std::function<void(const toolkit::SockException &ex)> _on_play;
    std::function<void(const std::string &proxyKey, const float &progress)> _on_replay_close;
    std::function<bool(const std::string &proxyKey, const float &progress, std::string &newUrl)> _on_replay_retry;
    TranslationInfo _transtalion_info;
    MultiMediaSourceMuxer::Ptr _muxer;

    toolkit::Ticker _live_ticker;
    // 0 indicates normal, 1 indicates attempting to stream
    std::atomic<int> _live_status;
    std::atomic<uint64_t> _live_secs;

    std::atomic<uint64_t> _repull_count;
    time_t _replay_recoder_time_file;
};

} /* namespace mediakit */
#endif /* SRC_DEVICE_PLAYERPROXY_H_ */
