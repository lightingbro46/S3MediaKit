#ifndef ZLMEDIAKIT_MULTIMEDIASOURCEMUXER_H
#define ZLMEDIAKIT_MULTIMEDIASOURCEMUXER_H

#include "Common/Stamp.h"
#include "Common/MediaSource.h"
#include "Common/MediaSink.h"
#include "Record/Recorder.h"
#include "Rtp/RtpSender.h"
#include "Record/HlsRecorder.h"
#include "Record/HlsMediaSource.h"
#include "Rtsp/RtspMediaSourceMuxer.h"
#include "Rtmp/RtmpMediaSourceMuxer.h"
#include "TS/TSMediaSourceMuxer.h"
#include "FMP4/FMP4MediaSourceMuxer.h"

namespace mediakit {

class MultiMediaSourceMuxer : public MediaSourceEventInterceptor, public MediaSink, public std::enable_shared_from_this<MultiMediaSourceMuxer>{
public:
    using Ptr = std::shared_ptr<MultiMediaSourceMuxer>;
    using RingType = toolkit::RingBuffer<Frame::Ptr>;

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void onAllTrackReady() = 0;
    };

    MultiMediaSourceMuxer(const MediaTuple& tuple, float dur_sec = 0.0,const ProtocolOption &option = ProtocolOption());

    /**
     * Set event listener
     * @param listener Listener
     */
    void setMediaListener(const std::weak_ptr<MediaSourceEvent> &listener);

     /**
      * Set Track ready event listener
      * @param listener Event listener
     */
    void setTrackListener(const std::weak_ptr<Listener> &listener);

    /**
     * Return the total number of consumers
     */
    int totalReaderCount() const;

    /**
     * Determine whether it is effective (whether it is being converted to another protocol)
     */
    bool isEnabled();

    /**
     * Set MediaSource timestamp
     * @param stamp Timestamp
     */
    void setTimeStamp(uint32_t stamp);

    /**
     * Reset track
     */
    void resetTracks() override;

    /////////////////////////////////MediaSourceEvent override/////////////////////////////////

    /**
     * Total number of viewers
     * @param sender Event sender
     * @return Total number of viewers
     */
    int totalReaderCount(MediaSource &sender) override;

    /**
     * Set recording status
     * @param type Recording type
     * @param start Start or stop
     * @param custom_path Specify a custom path when recording is enabled
     * @return Whether the setting is successful
     */
    bool setupRecord(MediaSource &sender, Recorder::type type, bool start, const std::string &custom_path, size_t max_second) override;

    /**
     * Get recording status
     * @param type Recording type
     * @return Recording status
     */
    bool isRecording(MediaSource &sender, Recorder::type type) override;

    /**
     * Start sending ps-rtp stream
     * @param dst_url Target ip or domain name
     * @param dst_port Target port
     * @param ssrc rtp's ssrc
     * @param is_udp Whether it is udp
     * @param cb Start success or failure callback
     */
    void startSendRtp(MediaSource &sender, const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb) override;

    /**
     * Stop ps-rtp sending
     * @return Whether it is successful
     */
    bool stopSendRtp(MediaSource &sender, const std::string &ssrc) override;

    /**
     * Get all Tracks
     * @param trackReady Whether to filter out unready tracks
     * @return All Tracks
     */
    std::vector<Track::Ptr> getMediaTracks(MediaSource &sender, bool trackReady = true) const override;

    /**
     * Get the thread it belongs to
     */
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;

    /**
     * Get this object
     */
    std::shared_ptr<MultiMediaSourceMuxer> getMuxer(MediaSource &sender) const override;

    const ProtocolOption &getOption() const;
    const MediaTuple &getMediaTuple() const;
    std::string shortUrl() const;

    void forEachRtpSender(const std::function<void(const std::string &ssrc)> &cb) const;

protected:
    /////////////////////////////////MediaSink override/////////////////////////////////

    /**
     * A certain track is ready, its ready() status returns true,
     * This means that you can get information such as sps pps, etc.
     * @param track
    */
    bool onTrackReady(const Track::Ptr & track) override;

    /**
     * All Tracks are ready,
     */
    void onAllTrackReady() override;

    /**
     * A certain Track outputs a frame, this method will be called after onAllTrackReady is triggered
     * @param frame
     */
    bool onTrackFrame(const Frame::Ptr &frame) override;
    bool onTrackFrame_l(const Frame::Ptr &frame);

private:
    void createGopCacheIfNeed(size_t gop_count);
    std::shared_ptr<MediaSinkInterface> makeRecorder(MediaSource &sender, Recorder::type type);

private:
    bool _is_enable = false;
    bool _create_in_poller = false;
    bool _video_key_pos = false;
    float _dur_sec;
    std::shared_ptr<class FramePacedSender> _paced_sender;
    MediaTuple _tuple;
    ProtocolOption _option;
    toolkit::Ticker _last_check;
    std::unordered_map<int, Stamp> _stamps;
    std::weak_ptr<Listener> _track_listener;
    std::unordered_multimap<std::string, RingType::RingReader::Ptr> _rtp_sender;
    FMP4MediaSourceMuxer::Ptr _fmp4;
    RtmpMediaSourceMuxer::Ptr _rtmp;
    RtspMediaSourceMuxer::Ptr _rtsp;
    TSMediaSourceMuxer::Ptr _ts;
    MediaSinkInterface::Ptr _mp4;
    HlsRecorder::Ptr _hls;
    HlsFMP4Recorder::Ptr _hls_fmp4;
    toolkit::EventPoller::Ptr _poller;
    RingType::Ptr _ring;

    // Object count statistics
    toolkit::ObjectStatistic<MultiMediaSourceMuxer> _statistic;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_MULTIMEDIASOURCEMUXER_H
