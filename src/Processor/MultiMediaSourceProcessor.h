#ifndef MULTI_MEDIASOURCE_PROCESSOR_H
#define MULTI_MEDIASOURCE_PROCESSOR_H

#if defined(ENABLE_FFMPEG)
#include "MediaSourceDecoder.h"

#if defined(ENABLE_MOTION)
#include "Motion/MotionProcessor.h"
#include "Motion/MotionMjpegMediaSourceMuxer.h"
#endif // ENABLE_MOTION

#include "Transcode/TranscodeProcessor.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mediakit {

class MultiMediaSourceProcessor
    : public MediaSourceDecoder
    , public MediaSourceEventInterceptor
    , public std::enable_shared_from_this<MultiMediaSourceProcessor> {
public:
    using Ptr = std::shared_ptr<MultiMediaSourceProcessor>;

    MultiMediaSourceProcessor(const MediaTuple &tuple, const ProtocolOption &option,
                              const toolkit::EventPoller::Ptr &poller = nullptr);

    void setListener(const std::weak_ptr<MediaSourceEvent> &listener);

    bool addTrack(const Track::Ptr &track) override;

    bool inputFrame(const Frame::Ptr &frame) override;

    /** Create or reuse a transcode variant while sharing this processor's decoder. */
    MediaSource::Ptr ensureTranscode(const TranscodeProcessor::Config &cfg);

    /** Whether any motion/transcode output is active for its demand policy. */
    bool isEnabled();
    bool canClose() const;
    void setOnIdle(const std::function<void()> &callback);

    void addTrackCompleted() override;

    void resetTracks() override;

    int readerCount() const;

    bool isMotionDetectRunning();

protected:
    void onDecode(const FFmpegFrame::Ptr &frame) override;
    void onReaderChanged(MediaSource &sender, int size) override;

private:
    TranscodeProcessor::Ptr createTranscode(const TranscodeProcessor::Config &cfg);

    /** Remove a closed transcode variant if it is still the mapped instance. */
    void removeTranscode(const std::string &key, const TranscodeProcessor::Ptr &transcode);
    void attachTranscodeReader(const std::string &key, const TranscodeProcessor::Ptr &transcode);
    void attachMotionReader();
    std::vector<TranscodeProcessor::Ptr> snapshotTranscodes() const;

    MediaTuple _tuple;
    ProtocolOption _option;
    toolkit::EventPoller::Ptr _poller;
    toolkit::EventPoller::Ptr _transcode_poller;
    struct DecodedFrame {
        enum Type {
            Video,
            Audio
        } type = Video;

        FFmpegFrame::Ptr video;
        Frame::Ptr audio;
    };
    using RingType = toolkit::RingBuffer<DecodedFrame>;

#if defined(ENABLE_MOTION)
    MotionProcessor::Ptr _motion;
    MotionMjpegMediaSourceMuxer::Ptr _mjpeg_muxer;
    RingType::RingReader::Ptr _motion_reader;
    toolkit::EventPoller::Ptr _motion_poller;
#endif // ENABLE_MOTION

    std::unordered_map<std::string, TranscodeProcessor::Ptr> _transcodes;
    RingType::Ptr _ring;
    std::unordered_map<std::string, RingType::RingReader::Ptr> _transcode_readers;
    mutable std::mutex _mtx;
    // Exactly one source audio track is selected for every derived transcode.
    // A real track replaces the synthetic 0xffff fallback before tracks are
    // finalized; TranscodeProcessor never creates another mute track.
    Track::Ptr _selected_audio_track;
    double _source_video_fps = 0.0;
    bool _tracks_completed = false;
    bool _source_on_demand = false;
    std::function<void()> _on_idle;
    toolkit::Timer::Ptr _idle_timer;
};

} // namespace mediakit

#endif // ENABLE_FFMPEG
#endif // MULTI_MEDIASOURCE_PROCESSOR_H
