#ifndef MOTION_MOTIONEVENTCONTROLLER_H
#define MOTION_MOTIONEVENTCONTROLLER_H

#ifdef ENABLE_MOTION

#include <functional>
#include "Common/MediaSource.h"
#include "MotionDetector.h"
#include "MotionMuxer.h"

namespace mediakit {

class MotionEventController {
public:
    using Ptr = std::shared_ptr<MotionEventController>;

    MotionEventController(const MediaTuple &tuple, int min_duration_ms = 1000);
    ~MotionEventController();

    /**
     * Attach a muxer. The controller drives it directly:
     *   - relevant frames → muxer.inputEvent()
     *   - motion confirmed → muxer.setRecording(true)
     *   - motion ended    → muxer.setRecording(false)
     *   - debounce reset  → muxer.clearPreBuffer()
     */
    void setMuxer(std::weak_ptr<MotionMuxer> muxer) { _muxer = std::move(muxer); }

    void inputBlock(bool motion_detected, uint64_t pts_ms, const MotionBitmapPtr &result = nullptr);
    void flush();

private:
    void emitMotionEvent(bool start);
    void setRecording(bool recording);
    void clearPreBuffer(bool motion, uint64_t pts_ms, const MotionBitmapPtr &result);
    void feedMuxer(uint64_t stamp, const MotionBitmapPtr &result);

private:
    int  _min_duration_ms;
    bool _in_motion = false;
    std::weak_ptr<MotionMuxer> _muxer;

    uint64_t _tmp_start_ms = 0;
    uint64_t _tmp_end_ms   = 0;

    struct MotionEvent : public MediaTuple {
        uint64_t start_time;
        uint64_t end_time;
    };
    MotionEvent _info;
};

} // namespace mediakit

#endif // ENABLE_MOTION

#endif //MOTION_MOTIONEVENTCONTROLLER_H