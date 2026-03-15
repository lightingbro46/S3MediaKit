#ifndef MOTION_MOTIONMJPEGMEDIASOURCEMUXER_H
#define MOTION_MOTIONMJPEGMEDIASOURCEMUXER_H

#include "Mjpeg/MjpegMediaSourceMuxer.h"
#include "MotionMjpegMediaSource.h"

namespace mediakit {

/**
 * Motion-specific MJPEG muxer.
 *
 * Extends MjpegMediaSourceMuxer<MotionMjpegMediaSource> with:
 *   - onWrite()    — push a pre-encoded MotionJpegFrame to connected viewers.
 *   - setOverlay() — toggle per-frame visual overlays (motion cells, ROI border).
 *
 * Demand gating (motion_demand flag) is handled entirely by the base class.
 *
 * Lifecycle:
 *   1.  auto muxer = std::make_shared<MotionMjpegMediaSourceMuxer>(tuple, option);
 *   2.  muxer->setListener(shared_from_this());   // after make_shared
 *   3.  MotionProcessor calls muxer->isEnabled() + muxer->onWrite(pkt)
 *   4.  HTTP clients connect via GET *.motion.mjpeg → MediaSource::findAsync
 */
class MotionMjpegMediaSourceMuxer final
    : public MjpegMediaSourceMuxer<MotionMjpegMediaSource> {
public:
    using Ptr  = std::shared_ptr<MotionMjpegMediaSourceMuxer>;
    using Base = MjpegMediaSourceMuxer<MotionMjpegMediaSource>;

    MotionMjpegMediaSourceMuxer(const MediaTuple &tuple, const ProtocolOption &option)
        : Base(std::make_shared<MotionMjpegMediaSource>(tuple), option.motion_demand) {}

    ~MotionMjpegMediaSourceMuxer() override = default;

    /**
     * Push a pre-encoded MJPEG frame to all connected readers.
     * Should only be called when isEnabled() returns true.
     */
    void onWrite(const MotionJpegFrame::Ptr &frame) {
        _media_src->onWrite(frame);
    }

    /**
     * Toggle overlays drawn on each live frame in realtime (thread-safe).
     * @param overlay_motion  Highlight motion-active grid cells.
     * @param overlay_roi     Draw ROI mask border.
     */
    void setOverlay(bool overlay_motion, bool overlay_roi) {
        _media_src->setOverlay(overlay_motion, overlay_roi);
    }

    bool overlayMotion() const { return _media_src->overlayMotion(); }
    bool overlayRoi()    const { return _media_src->overlayRoi();    }
};

} // namespace mediakit

#endif // MOTION_MOTIONMJPEGMEDIASOURCEMUXER_H
