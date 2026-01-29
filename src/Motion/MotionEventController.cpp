#include "MotionEventController.h"
#include "Common/config.h"
#include "Util/NoticeCenter.h"

using namespace std;
using namespace toolkit;

namespace mediakit {
    
MotionEventController::MotionEventController(const MediaTuple &tuple, int min_duration_ms)
    : _min_duration_ms(min_duration_ms) {
    static_cast<MediaTuple&>(_info) = tuple;
}

MotionEventController::~MotionEventController() {
    if (_in_motion) {
        DebugL << "Destructor: Motion ended!";
        emitMotionEvent(false);
    }
    _in_motion = false;
}

void MotionEventController::onMotionDetected(bool motion_detected, double ratio, uint64_t pts_ms) {
    if (motion_detected) {
        _last_motion_ms = pts_ms;
        if (!_in_motion) {
            if (_tmp_motion_start_ms == 0) {
                _tmp_motion_start_ms = pts_ms;
                return;
            }
            uint64_t motion_duration = pts_ms - _tmp_motion_start_ms;
            if (motion_duration < static_cast<uint64_t>(_min_duration_ms)) {
                // Ignore short idle durations
                // DebugL << "Ignoring short idle duration: " << motion_duration << " ms";
                return;
            }
            // Motion duration meets the minimum requirement
            _in_motion = true;
            _info.start_time = time(nullptr);
            DebugL << "Motion started! At" << getTimeStr("%Y-%m-%d %H:%M:%S", _info.start_time / 1000);
            emitMotionEvent(true, motion_duration);
        } else {
            // Already in motion, just update last motion time
            // DebugL << "Motion continues at" << getTimeStr("%Y-%m-%d %H:%M:%S", pts_ms / 1000);
        }
    } else {
        if (_in_motion) {
            // Check if the idle duration meets the minimum requirement
            uint64_t idle_duration = pts_ms - _last_motion_ms;
            if (idle_duration >= static_cast<uint64_t>(_min_duration_ms)) {
                _in_motion = false;
                _info.end_time = time(nullptr);
                DebugL << "Motion ended! At" << getTimeStr("%Y-%m-%d %H:%M:%S", _info.end_time / 1000);
                emitMotionEvent(false, idle_duration);
                _tmp_motion_start_ms = 0;
            }
        } else {
            // Not in motion, reset temporary start time
            _tmp_motion_start_ms = 0;
        }
    }
}

void MotionEventController::emitMotionEvent(bool start, int pre_ms) {
    auto flag = NOTICE_EMIT(BroadcastMediaMotionChangedArgs, Broadcast::kBroadcastMediaMotionChanged, static_cast<MediaTuple&>(_info), start, pre_ms);
    if (!flag) {
        DebugL << "Nobody listen on kBroadcastMediaMotionChanged event";
    }
    
    if (!start) {
        // Emit record motion event
        auto flag1 = NOTICE_EMIT(BroadcastRecordMotionArgs, Broadcast::kBroadcastRecordMotion, _info);
        if (!flag1) {
            DebugL << "Nobody listen on kBroadcastRecordMotion event";
        }
    }
}

} // namespace mediakit
