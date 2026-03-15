#include "MultiMediaSourceProcessor.h"
#include "Common/MultiMediaSourceMuxer.h"

using namespace std;

namespace mediakit {

MultiMediaSourceProcessor::MultiMediaSourceProcessor(const MediaTuple &tuple, const ProtocolOption &option) : _tuple(tuple), _option(option) {}

void MultiMediaSourceProcessor::setListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);
    if (_mjpeg_muxer) {
        _mjpeg_muxer->setListener(shared_from_this());
    }
}

void MultiMediaSourceProcessor::addTrackCompleted() {
    if (haveVideo()) {
        if (_option.enable_motion) {
            GET_CONFIG(int, interval_ms, Motion::kIntervalMS);
            GET_CONFIG(bool, use_y_channel, Motion::kUseYChannel);
            _motion = std::make_shared<MotionProcessor>(_tuple, _option.roi_mask, _option.record_motion, interval_ms, use_y_channel);
            _motion->setListener(shared_from_this());

            // Create the live MJPEG muxer only when enabled (either always-on or demand mode).
            // motion_demand=true  → source becomes active only when a viewer connects.
            // motion_demand=false → source is always active once motion detection starts.
            _mjpeg_muxer = std::make_shared<MotionMjpegMediaSourceMuxer>(_tuple, _option);
            _motion->setMjpegMuxer(_mjpeg_muxer);
            // Listener will be set (or updated) when setListener() is called by MultiMediaSourceMuxer.
        }
    }
}
   
void MultiMediaSourceProcessor::onDecode(const FFmpegFrame::Ptr &frame) {
    TraceL << "Decoded frame dts: " << frame->get()->pkt_dts << ", pts: " << frame->get()->pts << ", size: " << frame->get()->pkt_size;
    // Dispatch decoded frame to all tracks
    if (_motion) {
        _motion->inputFrame(frame);
    }
}

bool MultiMediaSourceProcessor::isMotionDetectRunning() {
    return !!_motion;
}

} // namespace mediakit
