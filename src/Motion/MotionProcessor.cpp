#include "MotionProcessor.h"
#include "Common/config.h"
#include "Processor/MultiMediaSourceProcessor.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MotionProcessor::MotionProcessor(const MediaTuple &tuple, const string &roi_mask, bool enable_record, int interval_ms, bool use_y_channel) 
    : _tuple(tuple), _roi_mask(roi_mask), _enable_record(enable_record), _interval_ms(interval_ms), _use_y_channel(use_y_channel) {
    GET_CONFIG(int, min_duration, Motion::kMinDurationMS);
    _recorder = std::make_shared<MotionRecorder>(tuple, enable_record, min_duration);
}

MotionProcessor::~MotionProcessor() {
    if (_recorder) {
        _recorder->flush();
        _recorder.reset();
    }
}

void MotionProcessor::setListener(const std::weak_ptr<MultiMediaSourceProcessor> &delegate) {
    _delegate = delegate;
}

bool MotionProcessor::inputFrame(const FFmpegFrame::Ptr &frame) {
    FFmpegFrame::Ptr proc_frame = frame;
    if (!_use_y_channel) {
        // Convert to grayscale frame
        swsGrayScale(frame, proc_frame);
        if (!proc_frame) {
            WarnL << "Failed to convert frame to grayscale";
            return false;
        }
    }

    auto ref = proc_frame->get();
    // On first frame, initialize detector with frame dimensions
    if (!_detector) {
        auto frame_width = ref->width;
        auto frame_height = ref->height;
        ROIMaskPtr roi_mask = make_shared<ROIMask>(MOTION_GRID_ROWS, MOTION_GRID_COLS, _roi_mask);
        _detector = std::make_shared<MotionDetector>(frame_width, frame_height, roi_mask);
        std::weak_ptr<MotionProcessor> weak_self = shared_from_this();
        _detector->setOnMotionResultCallback([weak_self, roi_mask](bool motion, uint64_t pts_ms, const MotionBitmapPtr &result) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->recordMotionResult(motion, pts_ms, result);

            // Save frame when motion is detected, for debugging or recording purposes
            // if (motion) {
            //     auto last_frame = strong_self->_last_frame;
            //     strong_self->saveFrame(last_frame, result, true, roi_mask, true);
            // }
        });
    }

    uint64_t pts_ms = ref->pts;
    // Throttling: Only process frames at the specified interval to reduce CPU usage
    if (pts_ms - _last_recv_time < static_cast<uint64_t>(_interval_ms)) {
        return true;
    }
    _last_recv_time = pts_ms;
    // Save the last frame for potential use when motion is detected, to avoid the issue of the frame being modified by swsGrayScale and losing original data
    _last_frame = frame;

    // Process frame for motion detection
    return _detector->inputFrame(ref->data[0], ref->linesize[0], pts_ms);
}

void MotionProcessor::swsGrayScale(const FFmpegFrame::Ptr &in_frame, FFmpegFrame::Ptr &out_frame, bool gray_format) {
    if (!_sws_ctx) {
        _sws_ctx = std::make_shared<FFmpegSws>(gray_format ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_YUV420P, FRAME_SCALE_WIDTH, FRAME_SCALE_HEIGHT);
    }

    out_frame = _sws_ctx->inputFrame(in_frame);
}

void MotionProcessor::recordMotionResult(bool motion, uint64_t stamp, const MotionBitmapPtr &result) {
    if (_recorder) {
        _recorder->inputBlock(motion, stamp, result);
    }
}

void MotionProcessor::saveFrame(const FFmpegFrame::Ptr &frame, const MotionBitmapPtr &result, bool overlay_motion, const ROIMaskPtr &roi, bool overlay_roi) {
    // Clone the frame to avoid modifying the original frame data, 
    // which may be used for subsequent motion detection and could lead to incorrect results if modified directly
    auto clone_frame = frame->clone();
    if (!clone_frame) {
        WarnL << "Failed to clone frame for saving";
        return;
    }
    auto grid = _detector->getGridBoundary();
    WorkThreadPool::Instance().getPoller()->async([clone_frame, result, roi, overlay_roi, overlay_motion, grid]() {
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
        auto ret = FFmpegUtils::saveFrame(clone_frame, "./motion_detected.jpg");
        DebugL << "Frame saved: " << std::get<0>(ret) << ", " << std::get<1>(ret);
    });
}

} // namespace mediakit