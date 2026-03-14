#ifndef MOTION_MOTIONPROCESSOR_H
#define MOTION_MOTIONPROCESSOR_H

#include "MotionDetector.h"
#include "MotionEventController.h"
#include "MotionMuxer.h"
#include "Codec/Transcode.h"

namespace mediakit {

class MultiMediaSourceProcessor;
class MotionProcessor : public std::enable_shared_from_this<MotionProcessor> {
public:
    using Ptr = std::shared_ptr<MotionProcessor>;

    MotionProcessor(const MediaTuple &tuple, const std::string &roi_mask = "", bool enable_record = true, int interval_ms = 200, bool use_y_channel = false);
    ~MotionProcessor();

    /**
     * Input decoded frame for motion detection, the frame is expected to be in YUV420 format or GRAY8 format
     * @param frame Input frame
     * @return Whether the processing is successful (e.g., frame format is correct), motion detection results will be emitted via callback function set in MotionDetector
     */
    bool inputFrame(const FFmpegFrame::Ptr &frame);

    /**
     * Set delegate to receive motion events, e.g., log or trigger recording when motion is detected
     */
    void setListener(const std::weak_ptr<MultiMediaSourceProcessor> &delegate);

private:
    /**
     * Convert frame to down scaling, grayscale GRAY8 format if necessary
     * @param in_frame Input frame
     * @param out_frame Output frame
     */
    void swsGrayScale(const FFmpegFrame::Ptr &in_frame, FFmpegFrame::Ptr &out_frame, bool gray_format = true);

    /**
     * Record motion event, e.g., log motion ratio, motion area, etc.
     */
    void recordMotionResult(bool motion, uint64_t stamp, const MotionBitmapPtr &result);

    /**
     * Save frame when motion is detected, to debug or record
     */
    void saveFrame(const FFmpegFrame::Ptr &frame, const MotionBitmapPtr &result = nullptr, bool overlay_motion = false, const ROIMaskPtr &roi = nullptr, bool overlay_roi = false);

private:
    MediaTuple _tuple;
    std::string _roi_mask;
    bool _enable_record;
    int _interval_ms;
    bool _use_y_channel;
    bool _save_image = false;
    std::string _save_path;
    FFmpegSws::Ptr _sws_ctx;
    MotionDetector::Ptr _detector;
    MotionEventController::Ptr _event_ctr;
    MotionMuxer::Ptr _muxer;

    uint64_t _last_recv_time = 0;
    FFmpegFrame::Ptr _last_frame;
    std::weak_ptr<MultiMediaSourceProcessor> _delegate;
};

} // namespace mediakit

#endif //MOTION_MOTIONPROCESSOR_H