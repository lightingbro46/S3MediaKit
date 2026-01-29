#include "MotionProcessor.h"
#include "Common/config.h"

#define DEBUG_MOTION 1

using namespace std;
using namespace toolkit;

namespace mediakit {

MotionProcessor::MotionProcessor(const MediaTuple &tuple) : _tuple(tuple) {
    GET_CONFIG(int, interval_ms, Motion::kIntervalMS);
    GET_CONFIG(bool, use_y_channel, Motion::kUseYChannel);
    _use_y_channel = use_y_channel;
    _interval_ms = interval_ms;
}

MotionProcessor::~MotionProcessor() {}

bool MotionProcessor::inputFrame(const FFmpegFrame::Ptr &frame) {
    FFmpegFrame::Ptr proc_frame = frame;
    if (!_use_y_channel) {
        // Convert to grayscale frame
        swsGrayScale(frame, proc_frame);
        if (!proc_frame) {
            WarnL << "Failed to convert frame to grayscale";
            return true;
        }
    }

    // On first frame, initialize detector with frame dimensions
    if (!_detector) {
        createMotionDetector(proc_frame->get()->width, proc_frame->get()->height);
    }

    uint64_t pts_ms = proc_frame->get()->pts;
    // Check interval
    if (pts_ms - _last_detection_time < static_cast<uint64_t>(_interval_ms)) {
        return true;
    }
    _last_detection_time = pts_ms;

    // Process frame for motion detection
    auto result = _detector->processFrame(proc_frame->get()->data[0], proc_frame->get()->linesize[0], pts_ms);
    if (result.motion_detected) {
        // TraceL << "Motion detected! Ratio: " << result.ratio * 100 << "% at PTS: " << result.pts_ms;
#ifdef DEBUG_MOTION
        saveFrame(frame, result, true, true);
#endif
    }
    
    // Emit motion event
    emitMotionEvent(result);

    return true;
}

void MotionProcessor::swsGrayScale(const FFmpegFrame::Ptr &in_frame, FFmpegFrame::Ptr &out_frame, bool gray_format) {
    if (!_sws_ctx) {
        auto resolution = fitWidthHeight(in_frame->get()->width, in_frame->get()->height);
        _sws_ctx = std::make_shared<FFmpegSws>(gray_format ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_YUV420P, resolution.first, resolution.second);
    }

    out_frame = _sws_ctx->inputFrame(in_frame);
}

void MotionProcessor::emitMotionEvent(const MotionResult &result) {
    if (!_event_controller) {
        _event_controller = std::make_shared<MotionEventController>(_tuple);
    }
    _event_controller->onMotionDetected(result.motion_detected, result.ratio, result.pts_ms);
}

void MotionProcessor::createMotionDetector(int width, int height) {
    GET_CONFIG(int, block_size, Motion::kBlockSize);
    auto pixel_threshold = 0.1; // Could be made configurable
    _detector = std::make_shared<MotionDetector>(width, height, block_size, pixel_threshold);
    // todo: set ROI mask if needed
    TraceL << "MotionDetector created with size: " << width << "x" << height;
}

std::pair<int, int> MotionProcessor::fitWidthHeight(int width, int height) {
    // Ensure width and height are multiples of block size
    GET_CONFIG(int, block_size, Motion::kBlockSize);
    int adjusted_width = (width / block_size) * block_size;
    int adjusted_height = (height / block_size) * block_size;
    if (adjusted_height > 480) {
        adjusted_height = 480;
        adjusted_width = (adjusted_width * adjusted_height) / height;
        adjusted_width = (adjusted_width / block_size) * block_size;
    }
    return std::make_pair(adjusted_width, adjusted_height);
}

static vector<uint8_t> scaleROIMask(const vector<uint8_t> &src_mask, int src_w, int src_h, int dst_w, int dst_h) {
    vector<uint8_t> dst_mask(dst_w * dst_h, 0);
    for (int y = 0; y < dst_h; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            int src_x = x * src_w / dst_w;
            int src_y = y * src_h / dst_h;
            dst_mask[y * dst_w + x] = src_mask[src_y * src_w + src_x];
        }
    }
    return dst_mask;
}

static bool isBorder(int x, int y, const vector<uint8_t> &roi, int w, int h) {
    int idx = y*w + x;
    if (!roi[idx]) return false;

    if (x > 0     && !roi[idx - 1]) return true;
    if (x < w-1   && !roi[idx + 1]) return true;
    if (y > 0     && !roi[idx - w]) return true;
    if (y < h-1   && !roi[idx + w]) return true;
    return false;
}

static void overlayROIBorder(const FFmpegFrame::Ptr &frame, const vector<uint8_t> roi, int w = 0, int h = 0) {
    uint8_t* yPlane = frame->get()->data[0];
    int stride = frame->get()->linesize[0];

    int frame_width = frame->get()->width;
    int frame_height = frame->get()->height;

    for (int y = 1; y < frame_height - 1; ++y) {
        uint8_t* row = yPlane + y * stride;
        for (int x = 1; x < frame_width - 1; ++x) {
            if (isBorder(x, y, roi, w, h)) {
                row[x] = 255; // trắng
            }
        }
    }
}

static void overlayROISemiTransparent(const FFmpegFrame::Ptr &frame, const vector<uint8_t> roi, int w = 0, int h = 0) {
    uint8_t* yPlane = frame->get()->data[0];
    int stride = frame->get()->linesize[0];
    int frame_width = frame->get()->width;
    int frame_height = frame->get()->height;

    for (int y = 0; y < frame_height; ++y) {
        uint8_t* row = yPlane + y * stride;
        for (int x = 0; x < frame_width; ++x) {
            int idx = y*frame_width + x;
            if (roi[idx]) {
                // Tăng độ sáng pixel để làm nổi bật vùng ROI
                // row[x] = std::min(255, row[x] + 50); // Tăng giá trị Y lên 50, tối đa là 255
                row[x] = (row[x] * 7 + 255 * 3) / 10; // blend 30%
            }
        }
    }
}

void MotionProcessor::saveFrame(const FFmpegFrame::Ptr &frame, const MotionResult &result, bool overlay_roi, bool overlay_motion) {
    // if(overlay_roi && result.roi) {
    //     auto width = frame->get()->width;
    //     auto height = frame->get()->height;
    //     auto roi_fit_frame = scaleROIMask(result.roi->mask, result.roi->width, result.roi->height, width, height);
    //     overlayROISemiTransparent(frame, roi_fit_frame, width, height);
    // }

    // if(overlay_motion && result.motion_detected) {
    //     auto width = frame->get()->width;
    //     auto height = frame->get()->height;
    //     auto motion_map_fit = scaleROIMask(result.motion_map, result.roi->width, result.roi->height, width, height);
    //     overlayROISemiTransparent(frame, motion_map_fit, width, height);
    // }

    FFmpegUtils::saveFrame(frame, "./motion_detected.jpg", AV_PIX_FMT_YUVJ420P);
}

} // namespace mediakit