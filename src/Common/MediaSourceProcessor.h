#ifndef MEDIA_SOURCE_PROCESSOR_H
#define MEDIA_SOURCE_PROCESSOR_H

#include "Common/MediaSink.h"
#include "Common/MediaSource.h"
#include "Codec/Transcode.h"
#include "Motion/MotionProcessor.h"

namespace mediakit {

class MediaSourceProcessor final : public MediaSinkInterface, public std::enable_shared_from_this<MediaSourceProcessor> {
public:
    using Ptr = std::shared_ptr<MediaSourceProcessor>;

    MediaSourceProcessor(const MediaTuple &tuple, const ProtocolOption &option);
    ~MediaSourceProcessor() override;

    /**
     * Reset all Tracks
     */
    void resetTracks() override;

    /**
     * Input frame
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Refresh output all frame cache
     */
    void flush() override;

    /**
     * Add ready state track
     */
    bool addTrack(const Track::Ptr & track) override;

    /**
     * Track added completed
     */
    void addTrackCompleted() override;

private:
    /**
     * Decode callback
     */
    void onDecode(const FFmpegFrame::Ptr &frame);

private:
    MediaTuple _tuple;
    ProtocolOption _option;
    bool _have_video = false;
    FFmpegDecoder::Ptr _decoder;
    std::list<Track::Ptr> _tracks;
    MotionProcessor::Ptr _motion_proc;
};

} // namespace mediakit

#endif // MEDIA_SOURCE_PROCESSOR_H
