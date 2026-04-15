#if defined(ENABLE_FFMPEG)

#include "MultiMediaSourceProcessor.h"
#include "Common/MultiMediaSourceMuxer.h"

using namespace std;

namespace mediakit {

MultiMediaSourceProcessor::MultiMediaSourceProcessor(const MediaTuple &tuple, const ProtocolOption &option)
    : _tuple(tuple), _option(option) {
#if defined(ENABLE_MOTION)
    if (option.enable_motion) {
        GET_CONFIG(int, interval_ms, Motion::kIntervalMS);
        GET_CONFIG(bool, use_y_channel, Motion::kUseYChannel);
        _motion = std::make_shared<MotionProcessor>(tuple, option.roi_mask, option.record_motion, option.motion_record_stream_id, interval_ms, use_y_channel);
        // Create the live MJPEG muxer only when enabled (either always-on or demand mode).
        // motion_demand=true  → source becomes active only when a viewer connects.
        // motion_demand=false → source is always active once motion detection starts.
        _mjpeg_muxer = std::make_shared<MotionMjpegMediaSourceMuxer>(tuple, option);
    }
#else
    if (option.enable_motion) {
        WarnL << "Motion detection is not enabled. Please turn on the ENABLE_MOTION macro when compiling to use this feature.";
    }
#endif
}

void MultiMediaSourceProcessor::setListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);
#if defined(ENABLE_MOTION)
    if (_motion) {
        _motion->setListener(shared_from_this());
    }
    if (_mjpeg_muxer) {
        _mjpeg_muxer->setListener(shared_from_this());
    }
#endif // ENABLE_MOTION
}

void MultiMediaSourceProcessor::addTrackCompleted() {
#if defined(ENABLE_MOTION)
    if (haveVideo() && _motion && _mjpeg_muxer) {
        _motion->setMjpegMuxer(_mjpeg_muxer);
    }
#endif // ENABLE_MOTION
}
   
void MultiMediaSourceProcessor::onDecode(const FFmpegFrame::Ptr &frame) {
    TraceL << "Decoded frame dts: " << frame->get()->pkt_dts << ", pts: " << frame->get()->pts << ", size: " << frame->get()->pkt_size;
    // Dispatch decoded frame to all tracks
#if defined(ENABLE_MOTION)
    if (_motion) {
        _motion->inputFrame(frame);
    }
#endif // ENABLE_MOTION
}

bool MultiMediaSourceProcessor::isMotionDetectRunning() {
#if defined(ENABLE_MOTION)
    return !!_motion;
#else
    return false;
#endif // ENABLE_MOTION
}

} // namespace mediakit

#endif // ENABLE_FFMPEG