#ifndef ZLMEDIAKIT_MEDIASOURCE_H
#define ZLMEDIAKIT_MEDIASOURCE_H

#include <string>
#include <atomic>
#include <memory>
#include <functional>
#include "Util/mini.h"
#include "Network/Socket.h"
#include "Extension/Track.h"
#include "Record/Recorder.h"

namespace toolkit {
class Session;
} // namespace toolkit

namespace mediakit {

enum class MediaOriginType : uint8_t {
    unknown = 0,
    rtmp_push ,
    rtsp_push,
    rtp_push,
    pull,
    ffmpeg_pull,
    mp4_vod,
    device_chn,
    rtc_push,
    srt_push
};

std::string getOriginTypeString(MediaOriginType type);

class MediaSource;
class RtpProcess;
class MultiMediaSourceMuxer;
class MediaSourceEvent {
public:
    friend class MediaSource;

    class NotImplemented : public std::runtime_error {
    public:
        template<typename ...T>
        NotImplemented(T && ...args) : std::runtime_error(std::forward<T>(args)...) {}
    };

    virtual ~MediaSourceEvent() = default;

    // Get media source type
    virtual MediaOriginType getOriginType(MediaSource &sender) const { return MediaOriginType::unknown; }
    // Get media source url or file path
    virtual std::string getOriginUrl(MediaSource &sender) const;
    // Get media source client related information
    virtual std::shared_ptr<toolkit::SockInfo> getOriginSock(MediaSource &sender) const { return nullptr; }

    // Notify drag progress bar
    virtual bool seekTo(MediaSource &sender, uint32_t stamp) { return false; }
    // Notify pause or resume
    virtual bool pause(MediaSource &sender, bool pause) { return false; }
    // Notify multiple times
    virtual bool speed(MediaSource &sender, float speed) { return false; }
    // Notify it to stop generating streams
    virtual bool close(MediaSource &sender) { return false; }
    // Get the total number of viewers, this function is generally forced to overload
    virtual int totalReaderCount(MediaSource &sender) { throw NotImplemented(toolkit::demangle(typeid(*this).name()) + "::totalReaderCount not implemented"); }
    // Notify the change in the number of viewers
    virtual void onReaderChanged(MediaSource &sender, int size);
    // Stream registration or deregistration event
    virtual void onRegist(MediaSource &sender, bool regist) {}
    // Get packet loss rate
    virtual float getLossRate(MediaSource &sender, TrackType type) { return -1; }
    // Get the current thread, this function is generally forced to overload
    virtual toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) { throw NotImplemented(toolkit::demangle(typeid(*this).name()) + "::getOwnerPoller not implemented"); }

    // //////////////////////Only for MultiMediaSourceMuxer object inheritance////////////////////////
    // Start or stop recording
    virtual bool setupRecord(MediaSource &sender, Recorder::type type, bool start, const std::string &custom_path, size_t max_second) { return false; };
    // Get recording status
    virtual bool isRecording(MediaSource &sender, Recorder::type type) { return false; }
    // Get all track related information
    virtual std::vector<Track::Ptr> getMediaTracks(MediaSource &sender, bool trackReady = true) const { return std::vector<Track::Ptr>(); };
    // Get MultiMediaSourceMuxer object
    virtual std::shared_ptr<MultiMediaSourceMuxer> getMuxer(MediaSource &sender) const { return nullptr; }
    // Get RtpProcess object
    virtual std::shared_ptr<RtpProcess> getRtpProcess(MediaSource &sender) const { return nullptr; }

    class SendRtpArgs {
    public:
        enum DataType {
            kRtpES = 0, // Send ES stream
            kRtpPS = 1, // Send PS stream
            kRtpTS = 2 // Send TS stream
        };

        enum ConType {
            kTcpActive = 0, // TCP active mode, tcp client actively connects with the other party and sends rtp
            kUdpActive = 1, // UDP active mode, actively send data to the other party
            kTcpPassive = 2, // TCP passive mode, tcp server, wait for the other party to connect and reply to rtp
            kUdpPassive = 3, // UDP passive method, wait for the other party to send a Nat hole-punching package, and then reply to RTP to the source address of the hole-punching package
            kVoiceTalk = 4,  // Voice intercom mode, the other party must want to push the stream and reply to the rtp data through its push stream link.
        };

        // Rtp type
        DataType data_type = kRtpPS;
        // Connection type
        ConType con_type = kUdpActive;

        // Specify whether to send only pure audio stream when sending es stream
        bool only_audio = false;
        // rtp payload type
        uint8_t pt = 96;
        // Whether to support multiple servers sending with the same ssrc
        bool ssrc_multi_send = false;
        // Specify rtp ssrc
        std::string ssrc;
        // Specify local sending port
        uint16_t src_port = 0;
        // Send target port
        uint16_t dst_port;
        // Send target host address, can be ip or domain name
        std::string dst_url;

        // When sending udp, whether to enable rr rtcp receive timeout judgment
        bool udp_rtcp_timeout = false;
        // Passive passive, tcp active mode timeout time
        uint32_t close_delay_ms = 0;
        // When sending udp, rr rtcp packet receive timeout time, in milliseconds
        uint32_t rtcp_timeout_ms = 30 * 1000;
        // When sending udp, send sr rtcp packet interval, in milliseconds
        uint32_t rtcp_send_interval_ms = 5 * 1000;

        // Send rtp while receiving, generally used for two-way language intercom, if not empty, it means receiving is enabled
        std::string recv_stream_id;

        std::string recv_stream_app;
        std::string recv_stream_vhost;
    };

    // Start sending ps-rtp
    virtual void startSendRtp(MediaSource &sender, const SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb) { cb(0, toolkit::SockException(toolkit::Err_other, "not implemented"));};
    // Stop sending ps-rtp
    virtual bool stopSendRtp(MediaSource &sender, const std::string &ssrc) {return false; }

private:
    toolkit::Timer::Ptr _async_close_timer;
};


template <typename MAP, typename KEY, typename TYPE>
static void getArgsValue(const MAP &allArgs, const KEY &key, TYPE &value) {
    auto val = ((MAP &)allArgs)[key];
    if (!val.empty()) {
        value = (TYPE)val;
    }
}

template <typename KEY, typename TYPE>
static void getArgsValue(const toolkit::mINI &allArgs, const KEY &key, TYPE &value) {
    auto it = allArgs.find(key);
    if (it != allArgs.end()) {
        value = (TYPE)it->second;
    }
}

class ProtocolOption {
public:
    ProtocolOption();

    enum {
        kModifyStampOff = 0, // Adopt absolute timestamps for source video streams without any changes
        kModifyStampSystem = 1, // System timestamp when using s3mediakit to receive data (with smooth processing)
        kModifyStampRelative = 2 // Use the source video stream timestamp relative timestamp (growth volume), and perform timestamp jumps and fallback corrections.
    };
    // Timestamp type
    int modify_stamp;

    // Whether to enable audio for protocol conversion
    bool enable_audio;
    // Add mute audio, this switch is invalid when audio is closed
    bool add_mute_audio;
    // Whether to close directly when no one is watching (instead of returning close through the on_none_reader hook)
    // When this configuration is set to 1, if no one is watching this stream, it will not trigger the on_none_reader hook callback,
    // but will directly close the stream
    bool auto_close;

    // Delay in milliseconds for continuous pushing, default is using the configuration file
    uint32_t continue_push_ms;

    // Smooth sending timer interval, in milliseconds, set to 0 to close; enabling it will affect cpu performance and increase memory at the same time
    // This configuration can solve some problems where the stream is not sent smoothly, resulting in zlmediakit forwarding not being smooth
    uint32_t paced_sender_ms;

    // Whether to enable conversion to hls(mpegts)
    bool enable_hls;
    // Whether to enable conversion to hls(fmp4)
    bool enable_hls_fmp4;
    // Whether to enable MP4 recording
    bool enable_mp4;
    // Whether to enable conversion to rtsp/webrtc
    bool enable_rtsp;
    // Whether to enable conversion to rtmp/flv
    bool enable_rtmp;
    // Whether to enable conversion to http-ts/ws-ts
    bool enable_ts;
    // Whether to enable conversion to http-fmp4/ws-fmp4
    bool enable_fmp4;

    // Whether to generate hls protocol on demand, if hls.segNum is configured to 0 (meaning hls recording), then hls will always be generated (regardless of this switch)
    bool hls_demand;
    // Whether to generate rtsp[s] protocol on demand
    bool rtsp_demand;
    // Whether to generate rtmp[s]、http[s]-flv、ws[s]-flv protocol on demand
    bool rtmp_demand;
    // Whether to generate http[s]-ts protocol on demand
    bool ts_demand;
    // Whether to generate http[s]-fmp4、ws[s]-fmp4 protocol on demand
    bool fmp4_demand;

    // Whether to treat mp4 recording as a viewer
    bool mp4_as_player;
    // MP4 slice size, in seconds
    size_t mp4_max_second;
    // MP4 recording save path
    std::string mp4_save_path;

    // HLS recording save path
    std::string hls_save_path;

    // Support replacing stream_id through the return value of on_publish
    std::string stream_replace;

    // Maximum number of tracks
    size_t max_track = 2;

    template <typename MAP>
    ProtocolOption(const MAP &allArgs) : ProtocolOption() {
        load(allArgs);
    }

    template <typename MAP>
    void load(const MAP &allArgs) {
#define GET_OPT_VALUE(key) getArgsValue(allArgs, #key, key)
        GET_OPT_VALUE(modify_stamp);
        GET_OPT_VALUE(enable_audio);
        GET_OPT_VALUE(add_mute_audio);
        GET_OPT_VALUE(auto_close);
        GET_OPT_VALUE(continue_push_ms);
        GET_OPT_VALUE(paced_sender_ms);

        GET_OPT_VALUE(enable_hls);
        GET_OPT_VALUE(enable_hls_fmp4);
        GET_OPT_VALUE(enable_mp4);
        GET_OPT_VALUE(enable_rtsp);
        GET_OPT_VALUE(enable_rtmp);
        GET_OPT_VALUE(enable_ts);
        GET_OPT_VALUE(enable_fmp4);

        GET_OPT_VALUE(hls_demand);
        GET_OPT_VALUE(rtsp_demand);
        GET_OPT_VALUE(rtmp_demand);
        GET_OPT_VALUE(ts_demand);
        GET_OPT_VALUE(fmp4_demand);

        GET_OPT_VALUE(mp4_max_second);
        GET_OPT_VALUE(mp4_as_player);
        GET_OPT_VALUE(mp4_save_path);

        GET_OPT_VALUE(hls_save_path);
        GET_OPT_VALUE(stream_replace);
        GET_OPT_VALUE(max_track);
    }
};

// This object is used to intercept interesting MediaSourceEvent events
class MediaSourceEventInterceptor : public MediaSourceEvent {
public:
    void setDelegate(const std::weak_ptr<MediaSourceEvent> &listener);
    std::shared_ptr<MediaSourceEvent> getDelegate() const;

    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    std::shared_ptr<toolkit::SockInfo> getOriginSock(MediaSource &sender) const override;

    bool seekTo(MediaSource &sender, uint32_t stamp) override;
    bool pause(MediaSource &sender,  bool pause) override;
    bool speed(MediaSource &sender, float speed) override;
    bool close(MediaSource &sender) override;
    int totalReaderCount(MediaSource &sender) override;
    void onReaderChanged(MediaSource &sender, int size) override;
    void onRegist(MediaSource &sender, bool regist) override;
    bool setupRecord(MediaSource &sender, Recorder::type type, bool start, const std::string &custom_path, size_t max_second) override;
    bool isRecording(MediaSource &sender, Recorder::type type) override;
    std::vector<Track::Ptr> getMediaTracks(MediaSource &sender, bool trackReady = true) const override;
    void startSendRtp(MediaSource &sender, const SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb) override;
    bool stopSendRtp(MediaSource &sender, const std::string &ssrc) override;
    float getLossRate(MediaSource &sender, TrackType type) override;
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;
    std::shared_ptr<MultiMediaSourceMuxer> getMuxer(MediaSource &sender) const override;
    std::shared_ptr<RtpProcess> getRtpProcess(MediaSource &sender) const override;

private:
    std::weak_ptr<MediaSourceEvent> _listener;
};

/**
 * Parse the url to get media information
 */
class MediaInfo: public MediaTuple {
public:
    MediaInfo() = default;
    MediaInfo(const std::string &url) { parse(url); }
    void parse(const std::string &url);
    std::string getUrl() const { return schema + "://" + shortUrl(); }

public:
    uint16_t port = 0;
    std::string protocol;
    std::string full_url;
    std::string schema;
    std::string host;
};

bool equalMediaTuple(const MediaTuple& a, const MediaTuple& b);

/**
 * Media source, any rtsp/rtmp live stream originates from this object
 */
class MediaSource: public TrackSource, public std::enable_shared_from_this<MediaSource> {
public:
    static MediaSource& NullMediaSource();
    using Ptr = std::shared_ptr<MediaSource>;

    MediaSource(const std::string &schema, const MediaTuple& tuple);
    virtual ~MediaSource();

    // //////////////Get MediaSource information////////////////

    // Get protocol type
    const std::string& getSchema() const {
        return _schema;
    }

    const MediaTuple& getMediaTuple() const {
        return _tuple;
    }

    std::string getUrl() const { return _schema + "://" + _tuple.shortUrl(); }

    // Get object ownership
    std::shared_ptr<void> getOwnership();

    // Get all Tracks
    std::vector<Track::Ptr> getTracks(bool ready = true) const override;

    // Get the current timestamp of the stream
    virtual uint32_t getTimeStamp(TrackType type) { return 0; };
    // Set timestamp
    virtual void setTimeStamp(uint32_t stamp) {};

    // Get data rate, unit bytes/s
    int getBytesSpeed(TrackType type = TrackInvalid);
    // Get the stream creation GMT unix timestamp, unit seconds
    uint64_t getCreateStamp() const { return _create_stamp; }
    // Get the stream online time, unit seconds
    uint64_t getAliveSecond() const;

    // //////////////MediaSourceEvent related interface implementation////////////////

    // Set listener
    virtual void setListener(const std::weak_ptr<MediaSourceEvent> &listener);
    // Get listener
    std::weak_ptr<MediaSourceEvent> getListener() const;

    // This protocol gets the number of viewers, it may return the number of viewers of this protocol, or it may return the total number of viewers
    virtual int readerCount() = 0;
    // Number of viewers, including (hls/rtsp/rtmp)
    virtual int totalReaderCount();
    // Get the player list
    virtual void getPlayerList(const std::function<void(const std::list<toolkit::Any> &info_list)> &cb,
                               const std::function<toolkit::Any(toolkit::Any &&info)> &on_change) {
        assert(cb);
        cb(std::list<toolkit::Any>());
    }

    virtual bool broadcastMessage(const toolkit::Any &data) { return false; }

    // Get the media source type
    MediaOriginType getOriginType() const;
    // Get the media source url or file path
    std::string getOriginUrl() const;
    // Get the media source client information
    std::shared_ptr<toolkit::SockInfo> getOriginSock() const;

    // Drag the progress bar
    bool seekTo(uint32_t stamp);
    // Pause
    bool pause(bool pause);
    // Playback speed
    bool speed(float speed);
    // Close the stream
    bool close(bool force);
    // The number of viewers of this stream changes
    void onReaderChanged(int size);
    // Turn recording on or off
    bool setupRecord(Recorder::type type, bool start, const std::string &custom_path, size_t max_second);
    // Get recording status
    bool isRecording(Recorder::type type);
    // Start sending ps-rtp
    void startSendRtp(const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb);
    // Stop sending ps-rtp
    bool stopSendRtp(const std::string &ssrc);
    // Get packet loss rate
    float getLossRate(mediakit::TrackType type);
    // Get the thread where it is running
    toolkit::EventPoller::Ptr getOwnerPoller();
    // Get the MultiMediaSourceMuxer object
    std::shared_ptr<MultiMediaSourceMuxer> getMuxer() const;
    // Get the RtpProcess object
    std::shared_ptr<RtpProcess> getRtpProcess() const;

    // //////////////static methods, find or generate MediaSource////////////////

    // Synchronously find the stream
    static Ptr find(const std::string &schema, const std::string &vhost, const std::string &app, const std::string &id, bool from_mp4 = false);
    static Ptr find(const MediaInfo &info, bool from_mp4 = false) {
        return find(info.schema, info.vhost, info.app, info.stream, from_mp4);
    }

    // Ignore schema, synchronously find the stream, may return rtmp/rtsp/hls type
    static Ptr find(const std::string &vhost, const std::string &app, const std::string &stream_id, bool from_mp4 = false);

    // Asynchronously find the stream
    static void findAsync(const MediaInfo &info, const std::shared_ptr<toolkit::Session> &session, const std::function<void(const Ptr &src)> &cb);
    // Traverse all streams
    static void for_each_media(const std::function<void(const Ptr &src)> &cb, const std::string &schema = "", const std::string &vhost = "", const std::string &app = "", const std::string &stream = "");
    // Generate MediaSource from mp4 file
    static MediaSource::Ptr createFromMP4(const std::string &schema, const std::string &vhost, const std::string &app, const std::string &stream, const std::string &file_path = "", bool check_app = true);

protected:
    // Media registration
    void regist();

private:
    // Media unregistration
    bool unregist();
    // Trigger media events
    void emitEvent(bool regist);

protected:
    toolkit::BytesSpeed _speed[TrackMax];
    MediaTuple _tuple;

private:
    std::atomic_flag _owned { false };
    time_t _create_stamp;
    toolkit::Ticker _ticker;
    std::string _schema;
    std::weak_ptr<MediaSourceEvent> _listener;
    // Object count statistics
    toolkit::ObjectStatistic<MediaSource> _statistic;
};

} /* namespace mediakit */
#endif //ZLMEDIAKIT_MEDIASOURCE_H
