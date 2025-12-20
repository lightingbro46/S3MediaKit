#ifndef S3MEDIAKIT_RTPPROCESS_H
#define S3MEDIAKIT_RTPPROCESS_H

#if defined(ENABLE_RTPPROXY)
#include "ProcessInterface.h"
#include "Rtcp/RtcpContext.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {

static constexpr char kRtpAppName[] = "rtp";

class RtpProcess final : public RtcpContextForRecv, public toolkit::SockInfo, public MediaSinkInterface, public MediaSourceEvent, public std::enable_shared_from_this<RtpProcess>{
public:
    using Ptr = std::shared_ptr<RtpProcess>;
    using onDetachCB = std::function<void(const toolkit::SockException &ex)>;

    static Ptr createProcess(const MediaTuple &tuple);
    ~RtpProcess();
    enum OnlyTrack { kAll = 0, kOnlyAudio = 1, kOnlyVideo = 2 };

    /**
     * Input rtp
     * @param is_udp Whether it is udp mode
     * @param sock Local listening socket
     * @param data Rtp data pointer
     * @param len Rtp data length
     * @param addr Data source address
     * @param dts_out Parse out the latest dts
     * @return Whether the parsing is successful
     */
    bool inputRtp(bool is_udp, const toolkit::Socket::Ptr &sock, const char *data, size_t len, const struct sockaddr *addr , uint64_t *dts_out = nullptr);


    /**
     * Triggered when removed by RtpSelector when timeout
     */
    void onDetach(const toolkit::SockException &ex);

    /**
     * Set onDetach event callback
     */
    void setOnDetach(onDetachCB cb);

    /**
     *Pause or resume rtp timeout monitoring
     *@param pause whether to pause timeout detection
     *@param pause_seconds The maximum time for pausing timeout detection (in seconds). After this time, timeout detection will be resumed; when set to 0, the default is 300
     */
    void pauseRtpTimeout(bool pause, uint32_t pause_seconds = 0);

    /**
     * Set to single track, single audio/single video can speed up media registration
     * Please call this method before inputRtp, otherwise it may be a null operation
     */
    void setOnlyTrack(OnlyTrack only_track);

    /**
     * Flush output cache
     */
    void flush() override;

    /// SockInfo override
    std::string get_local_ip() override;
    uint16_t get_local_port() override;
    std::string get_peer_ip() override;
    uint16_t get_peer_port() override;
    std::string getIdentifier() const override;

    const toolkit::Socket::Ptr& getSock() const;

protected:
    bool inputFrame(const Frame::Ptr &frame) override;
    bool addTrack(const Track::Ptr & track) override;
    void addTrackCompleted() override;
    void resetTracks() override {};

    //// MediaSourceEvent override ////
    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    std::shared_ptr<SockInfo> getOriginSock(MediaSource &sender) const override;
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;
    float getLossRate(MediaSource &sender, TrackType type) override;
    Ptr getRtpProcess(mediakit::MediaSource &sender) const override;
    bool close(mediakit::MediaSource &sender) override;

private:
    RtpProcess(const MediaTuple &tuple);

    void emitOnPublish(uint32_t ssrc);
    void doCachedFunc();
    bool alive();
    void onManager();
    void createTimer();

private:
    bool _pause_timeout = false;
    uint32_t _pause_seconds = 5 * 60;
    uint64_t _dts = 0;
    uint64_t _total_bytes = 0;
    OnlyTrack _only_track = kAll;
    std::string _auth_err;
    std::unique_ptr<sockaddr_storage> _addr;
    toolkit::Socket::Ptr _sock;
    MediaInfo _media_info;
    toolkit::Ticker _last_frame_time;
    onDetachCB _on_detach;
    std::shared_ptr<FILE> _save_file_rtp;
    std::shared_ptr<FILE> _save_file_video;
    ProcessInterface::Ptr _process;
    MultiMediaSourceMuxer::Ptr _muxer;
    toolkit::Timer::Ptr _timer;
    toolkit::Ticker _last_check_alive;
    std::recursive_mutex _func_mtx;
    toolkit::Ticker _cache_ticker;
    std::deque<std::function<void()> > _cached_func;
};

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
#endif //S3MEDIAKIT_RTPPROCESS_H
