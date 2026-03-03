#ifndef S3MEDIAKIT_MULTIMEDIASOURCEMUXER_H
#define S3MEDIAKIT_MULTIMEDIASOURCEMUXER_H

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
#include "Processor/MultiMediaSourceProcessor.h"

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
    bool setupRecord(Recorder::type type, bool start, const std::string &custom_path, size_t max_second);

    /**
     * Start recording mp4
     * @param file_path mp4 relative path
     * @param back_time_ms Rewind recording duration
     * @param forward_time_ms Subsequent recording duration
     * @return Recording file absolute path
     */
    std::string startRecord(const std::string &file_path, uint32_t back_time_ms, uint32_t forward_time_ms);

    /**
     * Get recording status
     * @param type Recording type
     * @return Recording status
     */
    bool isRecording(Recorder::type type);

    /**
     * Start or stop motion detection, only for video streams
     */
    bool setupMotionDetect(bool start, bool record_motion = true, const std::string &custom_roi_mask = "");

    /**
     * Get motion detection status
      * @return Motion detection status
     */
    bool isMotionDetecting();

    /**
     *Start sending ps-rtp stream
     *@param cb startup success or failure callback
     */
    void startSendRtp(const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb);

    /**
     * Stop ps-rtp sending
     * @return Whether it is successful
     */
    bool stopSendRtp(const std::string &ssrc);

    /**
     * Get the thread it belongs to
     */
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;
    
    /**
    *Close the stream
     *@return whether successful
     */
    bool close(MediaSource &sender) override;

    /**
     * Get this object
     */
    std::shared_ptr<MultiMediaSourceMuxer> getMuxer(MediaSource &sender) const override;

    const ProtocolOption &getOption() const;
    const MediaTuple &getMediaTuple() const;
    std::string shortUrl() const;
#if defined(ENABLE_RTPPROXY)
    void forEachRtpSender(const std::function<void(const std::string &ssrc, const RtpSender &sender)> &cb) const;
#endif // ENABLE_RTPPROXY
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
    std::shared_ptr<MediaSinkInterface> makeRecorder(Recorder::type type);

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
#if defined(ENABLE_RTPPROXY)
    std::unordered_multimap<std::string, std::tuple<RingType::RingReader::Ptr, std::weak_ptr<RtpSender>>> _rtp_sender;
#endif // ENABLE_RTPPROXY
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

    // Module stack, used for motion detection module or other video processing modules
    MultiMediaSourceProcessor::Ptr _stack;
};

}//namespace mediakit
#endif //S3MEDIAKIT_MULTIMEDIASOURCEMUXER_H
