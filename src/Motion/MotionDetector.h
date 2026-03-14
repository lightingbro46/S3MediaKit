#ifndef S3MEDIAKIT_MOTIONDETECTOR_H
#define S3MEDIAKIT_MOTIONDETECTOR_H

#include "MotionBitmap.h"
#include <functional>

#define MOTION_GRID_ROWS 32
#define MOTION_GRID_COLS 44
#define FRAME_SCALE_WIDTH 440
#define FRAME_SCALE_HEIGHT 320

namespace mediakit {

class MotionDetector {
public:
    using Ptr = std::shared_ptr<MotionDetector>;
    using OnMotionResultCallback = std::function<void(bool motion, uint64_t stamp_ms, const MotionBitmapPtr &result)>;

    /**
     * Constructor
     * @param width Frame width
     * @param height Frame height
     * @param roi_mask Optional ROI mask string (format: "001234" where each character represents a pixel, '0' = ignore, '>1' = detect)
     * @param default_threshold Default threshold for motion detection, used when ROI level is not specified or invalid
     */
    MotionDetector(int width, int height, const ROIMaskPtr &roi_mask);
    ~MotionDetector();

    /**
     * Process a video frame for motion detection, using pixel differences
     * @param data Pointer to the frame data (YUV420 format or GRAY8 format)
     * @param linesize Line size of the frame
     * @param pts_ms Presentation timestamp in milliseconds
     * @return Whether the processing is successful (e.g., frame format is correct), motion detection results will be returned via callback function
     */
    bool inputFrame(const uint8_t* data, int linesize, uint64_t pts_ms);

    /**
     * Set callback function to receive motion detection results
     */
    void setOnMotionResultCallback(OnMotionResultCallback callback) { _on_result = std::move(callback); }

    /**
     * Get grid boundary information for motion visualization, 
     * which can be used to draw grid lines on the frame when overlaying motion detection results
     */
    GridBoundaryPtr getGridBoundary() const { return _grid_boundary; }

private:
    int _width;
    int _height;
    GridBoundaryPtr _grid_boundary;
    ROIMaskPtr _roi;
    bool _first_frame = true;
    std::vector<uint8_t> _prev_frame;
    OnMotionResultCallback _on_result;
};

} // namespace mediakit

#endif //S3MEDIAKIT_MOTIONDETECTOR_H