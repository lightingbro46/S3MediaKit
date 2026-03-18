#ifdef ENABLE_MOTION

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
    flush();
}

void MotionEventController::flush() {
    if (_in_motion) {
        _in_motion = false;
        _info.end_time = time(nullptr);
        DebugL << "Motion ended by flush! Stamp: " << getTimeStr("%Y-%m-%d %H:%M:%S", _info.end_time);
        emitMotionEvent(false);
    }
}

void MotionEventController::inputBlock(bool motion_detected, uint64_t stamp_ms, const MotionBitmapPtr &result) {
    const uint64_t base_ms = static_cast<uint64_t>(std::max(0, _min_duration_ms));
    // Hysteresis: To avoid frequent start/stop events due to short-term motion fluctuations, we use different thresholds for starting and stopping motion events
    const uint64_t start_confirm_ms = std::max<uint64_t>(base_ms, base_ms * 3 / 2); // 1.5x
    const uint64_t stop_confirm_ms  = base_ms;

    // Feed the muxer only for frames the controller considers relevant:
    // any frame with motion detected (debounce phase), or while an event is ongoing.
    if (motion_detected || _in_motion) {
        feedMuxer(stamp_ms, result);
    }

    if (motion_detected) {
        // Motion detected, check if we can start a motion event
        _tmp_end_ms = 0; // reset temporary stop timer

        if (_in_motion) {
            // Motion is already ongoing
            return;
        }

        // Debounce: Only start motion event if motion is continuously detected for at least start_confirm_ms
        if (_tmp_start_ms == 0) {
            _tmp_start_ms = stamp_ms;
            return;
        }

        const uint64_t motion_span_ms = stamp_ms - _tmp_start_ms;
        if (motion_span_ms < start_confirm_ms) {
            return;
        }

        // Start motion event
        _in_motion = true;
        _info.start_time = time(nullptr) - motion_span_ms / 1000; // Convert ms to seconds for timestamp
        _tmp_start_ms = 0; // Reset temporary start time

        DebugL << "Motion started! Stamp: " << getTimeStr("%Y-%m-%d %H:%M:%S", _info.start_time) << ", motion_span_ms: " << motion_span_ms;
        emitMotionEvent(true);
        return;
    }

    // No motion detected, check if we can stop the motion event
    _tmp_start_ms = 0;

    if (!_in_motion) {
        // Debounce-start interrupted — pre-buffered events are likely noise, discard them.
        
        return;
    }

    // Debounce: Only stop motion event if no motion is detected for at least stop_confirm_ms
    if (_tmp_end_ms == 0) {
        _tmp_end_ms = stamp_ms;
        return;
    }

    const uint64_t quiet_span_ms = stamp_ms - _tmp_end_ms;
    if (quiet_span_ms < stop_confirm_ms) {
        return;
    }

    // Stop motion event
    flush();
    _tmp_end_ms = 0; // Reset temporary end time
}

void MotionEventController::emitMotionEvent(bool start) {
    setRecording(start);
    auto flag = NOTICE_EMIT(BroadcastRecordMotionArgs, Broadcast::kBroadcastRecordMotion, static_cast<MediaTuple&>(_info), start);
    if (!flag) {
        DebugL << "Nobody listen on kBroadcastRecordMotion event";
    }
}

void MotionEventController::feedMuxer(uint64_t stamp_ms, const MotionBitmapPtr &result) {
    auto m = _muxer.lock();
    if (!m || !result) return;
    const size_t bitmap_bytes = static_cast<size_t>((result->rows * result->cols + 7) / 8);
    MotionEventExtHeader ext{};
    ext.rows         = static_cast<uint16_t>(result->rows);
    ext.cols         = static_cast<uint16_t>(result->cols);
    ext.active_cells = static_cast<uint16_t>(std::min(result->active_cells, 0xFFFF));
    std::vector<uint8_t> bitmap_vec(result->bitmap, result->bitmap + bitmap_bytes);
    MotionEventBlock block(stamp_ms, ext, std::move(bitmap_vec));
    m->inputEvent(block);
}

void MotionEventController::setRecording(bool recording) {
    if (auto m = _muxer.lock()) m->setRecording(recording);
}


void MotionEventController::clearPreBuffer(bool motion, uint64_t stamp_ms, const MotionBitmapPtr &result) {
    if (auto m = _muxer.lock()) m->clearPreBuffer();
}

} // namespace mediakit

#endif // ENABLE_MOTION
