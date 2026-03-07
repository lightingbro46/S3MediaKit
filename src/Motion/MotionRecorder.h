#ifndef MOTION_MOTIONEVENTCONTROLLER_H
#define MOTION_MOTIONEVENTCONTROLLER_H

#include <chrono>
#include "Common/MediaSource.h"
#include "MotionDetector.h"

namespace mediakit {

class MotionRecorder {
public:
    using Ptr = std::shared_ptr<MotionRecorder>;
    
    MotionRecorder(const MediaTuple &tuple, bool enable_record = true, int min_duration_ms = 1000);
    ~MotionRecorder();

    /**
     * Input motion detection result block, this function is expected to be called by MotionDetector when it has new motion detection results.
     * The function will handle the logic of starting/stopping motion events based on the detection results and duration, and emit motion events accordingly.
     */
    void inputBlock(bool motion_detected, uint64_t pts_ms, const MotionBitmapPtr &result = nullptr);

    /**
     * Flush motion event, e.g., when stream is closed, to ensure that any ongoing motion event is properly ended and recorded
     */
    void flush();

    /**
     * Save frame when motion is detected, to debug or record
     */
    void saveImage(const FFmpegFrame::Ptr &frame, const MotionBitmapPtr &result = nullptr, bool overlay_motion = false, const ROIMaskPtr &roi = nullptr, bool overlay_roi = false, const GridBoundaryPtr &grid = nullptr);

private:
    /**
     * Emit motion event through NoticeCenter, the event will be listened by other components to trigger actions such as recording or logging
     * @param start Whether this is a motion start event (true) or motion end event (false)
     */
    void emitMotionEvent(bool start);

private:
    bool _enable_record = true;
    int _min_duration_ms; // Minimum motion duration to trigger event
    bool _in_motion = false;

    std::string _path;
    std::string _full_path;

    // Temporary variable to track motion start time for duration calculation
    uint64_t _tmp_start_ms = 0;
    uint64_t _tmp_end_ms = 0;
    
    // Motion event information, can be extended with more fields if needed
    struct MotionEvent : public MediaTuple {
        uint64_t start_time; // Event start time (seconds since epoch)
        uint64_t end_time; // Event end time (seconds since epoch)
    };
    MotionEvent _info;

    uint32_t index = 0; // For generating unique file names when saving images
};

} // namespace mediakit

#endif //MOTION_MOTIONEVENTCONTROLLER_H