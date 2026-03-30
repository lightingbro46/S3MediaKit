#if defined(ENABLE_MOTION) && defined(ENABLE_FFMPEG)

#include "MotionProcessor.h"
#include "Common/config.h"
#include "Processor/MultiMediaSourceProcessor.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MotionProcessor::MotionProcessor(const MediaTuple &tuple, const string &roi_mask, bool enable_record, int interval_ms, bool use_y_channel) 
    : _tuple(tuple), _roi_mask(roi_mask), _interval_ms(interval_ms), _use_y_channel(use_y_channel) {
    if (_roi_mask.empty()) {
        GET_CONFIG(int, roi_level, Motion::kROIDefaultLevel);
        _roi_mask = string(MOTION_GRID_ROWS * MOTION_GRID_COLS, static_cast<char>('0' + roi_level));
    }
    // Whether to save frame when motion is detected, which is useful for debugging or recording
    GET_CONFIG(bool, save_image, Motion::kSaveImage);
    _save_image = save_image;

    // Build the motion recording base path (same layout as saveFrame).
    GET_CONFIG(string, record_path, Protocol::kMP4SavePath);
    GET_CONFIG(string, app_name, Record::kAppName);
    auto _record_path = File::absolutePath(app_name, record_path);
    GET_CONFIG(string, archive_name, Record::kArchiveName);
    GET_CONFIG(bool, enable_vhost, General::kEnableVhost);
    if (enable_vhost) {
        _save_path = _record_path + '/' + tuple.vhost + '/' + tuple.app + '/' + archive_name + "/motion/";
    } else {
        _save_path = _record_path + '/' + tuple.app + '/' + archive_name + "/motion/";
    }

    GET_CONFIG(int, min_duration, Motion::kMinDurationMS);
    _event_ctr = std::make_shared<MotionEventController>(tuple, min_duration);

    if (_enable_record) {
        GET_CONFIG(uint64_t, summary_window_ms, Motion::kSummaryWindowMS);
        _muxer = std::make_shared<MotionMuxer>(tuple, _roi_mask, _save_path, summary_window_ms);

        // Wire the muxer directly into the controller — single point of noise control.
        _event_ctr->setMuxer(_muxer);
    }
}

MotionProcessor::~MotionProcessor() {
    if (_muxer) {
        _muxer->flush();
        _muxer.reset();
    }
    if (_event_ctr) {
        _event_ctr->flush();
        _event_ctr.reset();
    }
}

void MotionProcessor::setMjpegMuxer(const std::shared_ptr<MotionMjpegMediaSourceMuxer> &muxer) {
    _mjpeg_muxer = muxer;
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
        _detector->setOnMotionResultCallback([weak_self, roi_mask](bool motion, uint64_t stamp_ms, const MotionBitmapPtr &result) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->recordMotionResult(motion, stamp_ms, result);

            // Save decoded frame for debugging or recording purposes
            if (strong_self->_save_image) {
                auto last_frame = strong_self->_last_frame;
                strong_self->saveFrame(last_frame, result, true, roi_mask, true);
            }

            // Push encoded MJPEG frame through the muxer if any client is connected
            auto mjpeg_muxer = strong_self->_mjpeg_muxer.lock();
            if (mjpeg_muxer && mjpeg_muxer->isEnabled()) {
                auto last_frame  = strong_self->_last_frame;
                auto bmp         = result;
                auto grid        = strong_self->_detector ? strong_self->_detector->getGridBoundary() : nullptr;
                bool ov_motion   = mjpeg_muxer->overlayMotion();
                bool ov_roi      = mjpeg_muxer->overlayRoi();
                auto weak_muxer  = std::weak_ptr<MotionMjpegMediaSourceMuxer>(mjpeg_muxer);
                bool is_motion   = motion;
                uint64_t pts     = stamp_ms;
                int cells        = bmp ? bmp->active_cells : 0;

                WorkThreadPool::Instance().getExecutor()->async([
                    last_frame, bmp, roi_mask, grid,
                    ov_motion, ov_roi, weak_muxer,
                    is_motion, pts, cells
                ]() {
                    auto mux = weak_muxer.lock();
                    if (!mux || !mux->isEnabled()) return;

                    auto clone = last_frame ? last_frame->clone() : nullptr;
                    if (!clone) return;

                    auto *avf = clone->get();
                    // Apply motion overlay (cell highlights)
                    if (ov_motion && bmp &&
                        (avf->format == AV_PIX_FMT_YUV420P || avf->format == AV_PIX_FMT_YUVJ420P)) {
                        GridBoundaryHelper::draw_motion_grid_yuv420p(avf, *bmp, grid, true, true);
                    }
                    // Apply ROI border overlay
                    if (ov_roi && roi_mask &&
                        (avf->format == AV_PIX_FMT_YUV420P || avf->format == AV_PIX_FMT_YUVJ420P)) {
                        GridBoundaryHelper::draw_roi_border_yuv420p(avf, *roi_mask, grid, true, true);
                    }

                    auto jpeg         = FFmpegUtils::encodeFrameToBuffer(clone);
                    auto pkt          = std::make_shared<MotionJpegFrame>();
                    pkt->motion       = is_motion;
                    pkt->stamp_ms     = pts;
                    pkt->active_cells = cells;
                    pkt->jpeg         = jpeg;
                    mux->onWrite(pkt);
                });
            }
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
    if (_event_ctr) {
        // The controller evaluates the frame and fires OnEvaluated → muxer internally.
        _event_ctr->inputBlock(motion, stamp, result);
    }
}

void MotionProcessor::saveFrame(const FFmpegFrame::Ptr &frame, const MotionBitmapPtr &result, bool overlay_motion, const ROIMaskPtr &roi, bool overlay_roi) {
    if (!frame) return;
    auto grid = _detector ? _detector->getGridBoundary() : nullptr;

    // Clone the frame to avoid modifying the original frame data,
    // which may be used for subsequent motion detection and could lead to incorrect results if modified directly
    auto clone_frame = frame->clone();
    if (!clone_frame) {
        WarnL << "Failed to clone frame for saving";
        return;
    }

    string full_path = StrPrinter << _save_path << "/" << "motion.jpg";

    WorkThreadPool::Instance().getExecutor()->async([clone_frame, result, roi, overlay_roi, overlay_motion, grid, full_path]() {
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

#endif // ENABLE_MOTION && ENABLE_FFMPEG