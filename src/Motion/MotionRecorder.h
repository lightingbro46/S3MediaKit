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

    void inputBlock(bool motion_detected, uint64_t pts_ms, const MotionBitmapPtr &result = nullptr);

    void flush();

private:
    void emitMotionEvent(bool start);

private:
    bool _enable_record = true;
    int _min_duration_ms; // Minimum motion duration to trigger event
    bool _in_motion = false;

    // Temporary variable to track motion start time for duration calculation
    uint64_t _tmp_start_ms = 0;
    uint64_t _tmp_end_ms = 0;
    
    // Motion event information, can be extended with more fields if needed
    struct MotionEvent : public MediaTuple {
        uint64_t start_time; // Event start time (seconds since epoch)
        uint64_t end_time; // Event end time (seconds since epoch)
    };
    MotionEvent _info;
};

} // namespace mediakit

#endif //MOTION_MOTIONEVENTCONTROLLER_H