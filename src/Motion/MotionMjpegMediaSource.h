#ifndef MOTION_MOTIONMJPEGMEDIASOURCE_H
#define MOTION_MOTIONMJPEGMEDIASOURCE_H

#ifdef ENABLE_MOTION

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "Mjpeg/MjpegMediaSource.h"
#include "Common/config.h"

namespace mediakit {

/**
 * A single encoded MJPEG frame produced by MotionProcessor.
 * Used as the FrameType for MjpegMediaSource<MotionJpegFrame>.
 */
struct MotionJpegFrame {
    using Ptr = std::shared_ptr<MotionJpegFrame>;

    bool     motion       = false; // true if motion was detected in this frame
    uint64_t stamp_ms     = 0;     // PTS in milliseconds
    int      active_cells = 0;     // number of motion-active grid cells
    // JPEG-encoded bytes; nullptr if encoding failed
    std::shared_ptr<std::vector<uint8_t>> jpeg;
};

/**
 * Live MJPEG media source for motion-analysis streams.
 * Extends MjpegMediaSource<MotionJpegFrame> with motion-specific overlay flags.
 *
 * Lifecycle: same as MjpegMediaSource — call startStream() after make_shared.
 *
 * Clients connect via:
 *   GET /media/{app}/{stream}.motion.mjpeg?overlay_motion=0|1&overlay_roi=0|1
 */
class MotionMjpegMediaSource final : public MjpegMediaSource<MotionJpegFrame> {
public:
    using Ptr      = std::shared_ptr<MotionMjpegMediaSource>;
    using RingType = MjpegMediaSource<MotionJpegFrame>::RingType;

    explicit MotionMjpegMediaSource(const MediaTuple &tuple, int ring_size = 10)
        : MjpegMediaSource<MotionJpegFrame>(MOTION_MJPEG_SCHEMA, tuple, ring_size) {}

    ~MotionMjpegMediaSource() override = default;

    /**
     * Toggle overlays drawn on each live MJPEG frame (thread-safe).
     * Setting is shared across all connected clients — last caller wins.
     *
     * @param motion  Highlight motion-active grid cells
     * @param roi     Draw ROI mask border
     */
    void setOverlay(bool motion, bool roi) {
        _overlay_motion.store(motion);
        _overlay_roi.store(roi);
    }

    bool overlayMotion() const { return _overlay_motion.load(); }
    bool overlayRoi()    const { return _overlay_roi.load();    }

private:
    std::atomic<bool> _overlay_motion{false};
    std::atomic<bool> _overlay_roi{false};
};

} // namespace mediakit

#endif // MOTION_MOTIONMJPEGMEDIASOURCE_H

#endif // ENABLE_MOTION
