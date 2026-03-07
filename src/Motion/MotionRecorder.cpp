#include "MotionRecorder.h"
#include "Common/config.h"
#include "Util/NoticeCenter.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;

namespace mediakit {
    
MotionRecorder::MotionRecorder(const MediaTuple &tuple, bool enable_record, int min_duration_ms)
    : _enable_record(enable_record), _min_duration_ms(min_duration_ms) {
    static_cast<MediaTuple&>(_info) = tuple;

    GET_CONFIG(string, record_path, Protocol::kMP4SavePath);
    GET_CONFIG(bool, enable_vhost, General::kEnableVhost);
    if (enable_vhost) {
        _path = record_path + "/motion/" + tuple.shortUrl() + '/';
    } else {
        _path = record_path + "/motion/" + tuple.app + '/' + tuple.stream + '/';
    }
}

MotionRecorder::~MotionRecorder() {
    flush();
}

void MotionRecorder::flush() {
    if (_in_motion && _enable_record) {
        _info.end_time = time(nullptr);
        DebugL << "Motion ended by flush! Stamp: " << getTimeStr("%Y-%m-%d %H:%M:%S", _info.end_time);
        //todo: record motion event in file or database
        emitMotionEvent(false);
    }
    _in_motion = false;
}

void MotionRecorder::inputBlock(bool motion_detected, uint64_t pts_ms, const MotionBitmapPtr &result) {
    const uint64_t base_ms = static_cast<uint64_t>(std::max(0, _min_duration_ms));
    // Hysteresis: To avoid frequent start/stop events due to short-term motion fluctuations, we use different thresholds for starting and stopping motion events
    const uint64_t start_confirm_ms = std::max<uint64_t>(base_ms, base_ms * 3 / 2); // 1.5x
    const uint64_t stop_confirm_ms  = base_ms;

    if (motion_detected) {
        // Motion detected, check if we can start a motion event
        _tmp_end_ms = 0;

        if (_in_motion) {
            // Motion is already ongoing
            return;
        }

        // Debounce: Only start motion event if motion is continuously detected for at least start_confirm_ms
        if (_tmp_start_ms == 0) {
            _tmp_start_ms = pts_ms;
            return;
        }

        const uint64_t motion_span_ms = pts_ms - _tmp_start_ms;
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
        // No ongoing motion event
        return;
    }

    // Debounce: Only stop motion event if no motion is detected for at least stop_confirm_ms
    if (_tmp_end_ms == 0) {
        _tmp_end_ms = pts_ms;
        return;
    }

    const uint64_t quiet_span_ms = pts_ms - _tmp_end_ms;
    if (quiet_span_ms < stop_confirm_ms) {
        return;
    }

    // Stop motion event
    _in_motion = false;
    _info.end_time = time(nullptr) - quiet_span_ms / 1000; // Convert ms to seconds for timestamp
    _tmp_end_ms = 0; // Reset temporary end time

    DebugL << "Motion ended! Stamp: " << getTimeStr("%Y-%m-%d %H:%M:%S", _info.end_time) << ", quiet_span_ms: " << quiet_span_ms;
    
    emitMotionEvent(false);
}

void MotionRecorder::emitMotionEvent(bool start) {
    if (!_enable_record) {
        return;
    }
    auto flag = NOTICE_EMIT(BroadcastRecordMotionArgs, Broadcast::kBroadcastRecordMotion, static_cast<MediaTuple&>(_info), start);
    if (!flag) {
        DebugL << "Nobody listen on kBroadcastRecordMotion event";
    }
}

void MotionRecorder::saveImage(const FFmpegFrame::Ptr &frame, const MotionBitmapPtr &result, bool overlay_motion, const ROIMaskPtr &roi, bool overlay_roi, const GridBoundaryPtr &grid) {
    // Clone the frame to avoid modifying the original frame data, 
    // which may be used for subsequent motion detection and could lead to incorrect results if modified directly
    auto clone_frame = frame->clone();
    if (!clone_frame) {
        WarnL << "Failed to clone frame for saving";
        return;
    }

    // string full_path = StrPrinter << _path << "/" << "motion_" << getTimeStr("%Y%m%d_%H%M%S") << "_" << index++ << ".jpg";
    string full_path = StrPrinter << _path << "/" << "motion.jpg";

    WorkThreadPool::Instance().getPoller()->async([clone_frame, result, roi, overlay_roi, overlay_motion, grid, full_path]() {
        if (overlay_motion && result) {
            if (clone_frame->get()->format == AV_PIX_FMT_YUV420P || clone_frame->get()->format == AV_PIX_FMT_YUVJ420P) {
                GridBoundaryHelper::draw_motion_grid_yuv420p(clone_frame->get(), *result, grid, true, true);
            }
        }

        if (overlay_roi && roi) {
            if (clone_frame->get()->format == AV_PIX_FMT_YUV420P || clone_frame->get()->format == AV_PIX_FMT_YUVJ420P) {
                GridBoundaryHelper::draw_roi_border_yuv420p(clone_frame->get(), *roi, grid, true, true);
            }
        }
        auto ret = FFmpegUtils::saveFrame(clone_frame, full_path.data());
        TraceL << "Frame saved: " << std::get<0>(ret) << ", " << std::get<1>(ret);
    });
}

} // namespace mediakit
