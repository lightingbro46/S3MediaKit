#ifndef MOTION_MOTIONPROCESSOR_H
#define MOTION_MOTIONPROCESSOR_H

#include "MotionDetector.h"
#include "MotionEventController.h"
#include "Codec/Transcode.h"

namespace mediakit {

class MotionProcessor {
public:
    using Ptr = std::shared_ptr<MotionProcessor>;

    MotionProcessor(const MediaTuple &tuple);
    ~MotionProcessor();

    bool inputFrame(const FFmpegFrame::Ptr &frame);

private:
    /**
     * Create motion detector
     */
    void createMotionDetector(int width, int height);

    /**
     * Convert frame to down scaling, grayscale GRAY8 format if necessary
     * @param in_frame Input frame
     * @param out_frame Output frame
     */
    void swsGrayScale(const FFmpegFrame::Ptr &in_frame, FFmpegFrame::Ptr &out_frame, bool gray_format = true);

    /**
     * Emit motion event, e.g., log or trigger recording
     * @param result Motion detection result
     */
    void emitMotionEvent(const MotionResult &result);

    /**
     * Convert width and height to fit motion detector requirements
     */
    std::pair<int, int> fitWidthHeight(int width, int height);

    /**
     * Save frame when motion is detected, to debug or record
     */
    void saveFrame(const FFmpegFrame::Ptr &frame, const MotionResult &result, bool overlay_roi = false, bool overlay_motion = false);

private:
    MediaTuple _tuple;
    bool _use_y_channel = true;
    int _interval_ms = 0;
    uint64_t _last_detection_time = 0;
    FFmpegSws::Ptr _sws_ctx;
    MotionDetector::Ptr _detector;
    MotionEventController::Ptr _event_controller;
};

} // namespace mediakit

#endif //MOTION_MOTIONPROCESSOR_H