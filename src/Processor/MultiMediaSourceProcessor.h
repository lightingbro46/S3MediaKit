
#ifndef MULTI_MEDIASOURCE_PROCESSOR_H
#define MULTI_MEDIASOURCE_PROCESSOR_H

#include "MediaSourceDecoder.h"
#include "Motion/MotionProcessor.h"
#include "Motion/MotionMjpegMediaSourceMuxer.h"

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

    void addTrackCompleted() override;

    bool isMotionDetectRunning();

protected:
    void onDecode(const FFmpegFrame::Ptr &frame) override;

private:
    MediaTuple _tuple;
    ProtocolOption _option;
    toolkit::EventPoller::Ptr _poller;
    std::unordered_map<int, std::weak_ptr<MultiMediaSourceMuxer>> _peer_muxers;

    MotionProcessor::Ptr _motion;
    MotionMjpegMediaSourceMuxer::Ptr _mjpeg_muxer;
};

} // namespace mediakit

#endif // MULTI_MEDIASOURCE_PROCESSOR_H
