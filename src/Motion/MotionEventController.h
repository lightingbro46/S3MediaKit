#ifndef MOTION_MOTIONEVENTCONTROLLER_H
#define MOTION_MOTIONEVENTCONTROLLER_H

#include <chrono>
#include "Common/MediaSource.h"

namespace mediakit {

struct MotionEvent : public MediaTuple {
    uint64_t start_time; // Event start time (seconds since epoch)
    uint64_t end_time; // Event end time (seconds since epoch)
    int motion_level; // Motion intensity level
    int motion_area; // Motion area description
};

class MotionEventController {
public:
    using Ptr = std::shared_ptr<MotionEventController>;
    
    MotionEventController(const MediaTuple &tuple, int min_duration_ms = 1000);
    ~MotionEventController();

    void onMotionDetected(bool motion_detected, double ratio, uint64_t pts_ms);

private:
    void emitMotionEvent(bool start, int pre_ms = 0);

private:
    int _min_duration_ms; // Minimum motion duration to trigger event
    bool _in_motion = false;
    uint64_t _tmp_motion_start_ms = 0;
    uint64_t _last_motion_ms = 0;
    MotionEvent _info;
};

} // namespace mediakit

#endif //MOTION_MOTIONEVENTCONTROLLER_H