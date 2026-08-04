#ifndef MULTI_MEDIASOURCE_PROCESSOR_H
#define MULTI_MEDIASOURCE_PROCESSOR_H

#if defined(ENABLE_FFMPEG)
#include "MediaSourceDecoder.h"

#if defined(ENABLE_MOTION)
#include "Motion/MotionProcessor.h"
#include "Motion/MotionMjpegMediaSourceMuxer.h"
#endif // ENABLE_MOTION

#include "Transcode/TranscodeProcessor.h"
#include <unordered_map>
#include <vector>

namespace mediakit {

class MultiMediaSourceMuxer;
class MultiMediaSourceProcessor
    : public MediaSourceDecoder
    , public MediaSourceEventInterceptor
    , public std::enable_shared_from_this<MultiMediaSourceProcessor> {
public:
    using Ptr = std::shared_ptr<MultiMediaSourceProcessor>;

    MultiMediaSourceProcessor(const MediaTuple &tuple, const ProtocolOption &option);

    void setListener(const std::weak_ptr<MediaSourceEvent> &listener);

    bool addTrack(const Track::Ptr &track) override;

    bool inputFrame(const Frame::Ptr &frame) override;

    /** Create or reuse a transcode variant while sharing this processor's decoder. */
    MediaSource::Ptr ensureTranscode(const TranscodeProcessor::Config &cfg);

    bool isTranscodeEnabled() const { return !_transcodes.empty(); }

    void addTrackCompleted() override;

    void resetTracks() override;

    bool isMotionDetectRunning();

protected:
    void onDecode(const FFmpegFrame::Ptr &frame) override;

private:
    TranscodeProcessor::Ptr createTranscode(const TranscodeProcessor::Config &cfg);

    MediaTuple _tuple;
    ProtocolOption _option;
    toolkit::EventPoller::Ptr _poller;
    std::unordered_map<int, std::weak_ptr<MultiMediaSourceMuxer>> _peer_muxers;

#if defined(ENABLE_MOTION)
    MotionProcessor::Ptr _motion;
    MotionMjpegMediaSourceMuxer::Ptr _mjpeg_muxer;
#endif // ENABLE_MOTION

    std::unordered_map<std::string, TranscodeProcessor::Ptr> _transcodes;
    std::vector<Track::Ptr> _audio_tracks;
    bool _tracks_completed = false;
};

} // namespace mediakit

#endif // ENABLE_FFMPEG
#endif // MULTI_MEDIASOURCE_PROCESSOR_H
