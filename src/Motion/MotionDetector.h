#ifndef S3MEDIAKIT_MOTIONDETECTOR_H
#define S3MEDIAKIT_MOTIONDETECTOR_H

#include <cstdint>
#include <vector>
#include <memory>

namespace mediakit {

struct ROIMask {
    int width; // Frame width after scale
    int height; // Frame height after scale
    std::vector<uint8_t> mask; // 1 byte = 1 pixel, 0 = ignore, 1 = detect

    ROIMask(int w, int h) : width(w), height(h), mask(w * h, 1) {}
};

using ROIMaskPtr = std::shared_ptr<ROIMask>;

struct MotionResult {
    bool motion_detected = false;
    double ratio = 0.0; // Ratio of motion area to total area
    uint64_t pts_ms = 0; // Presentation timestamp in milliseconds
    ROIMaskPtr roi; // Optional: ROI mask used
    std::vector<uint8_t> motion_map; // Optional: map of motion areas

    MotionResult(int w, int h) : motion_map(w * h, 0) {};
};

using MotionResultPtr = std::shared_ptr<MotionResult>;

struct Point {
    int x; // Top-left x coordinate
    int y; // Top-left y coordinate
};

using Polygon = std::vector<Point>;

class MotionDetector {
public:
    using Ptr = std::shared_ptr<MotionDetector>;

    /**
     * Constructor
     * @param width Frame width
     * @param height Frame height
     * @param block_size Size of the blocks to divide the frame into
     * @param threshold Threshold for motion detection in each block, range [0.0, 1.0]
     */
    MotionDetector(int width, int height, int block_size = 16, double threshold = 0.1);
    ~MotionDetector();

    /**
     * Process a video frame for motion detection, using pixel differences
     * @param data Pointer to the frame data (YUV420 format or GRAY8 format)
     * @param linesize Line size of the frame
     * @param pts_ms Presentation timestamp in milliseconds
     * @return Motion detection result
     */
    MotionResult processFrame(const uint8_t* data, int linesize, uint64_t pts_ms);
    /**
     * Set polygonal ROI mask
     */
    void setPolygonMask(const Polygon &p);

    /**
     * Set rectangular ROI mask
     */
    void setROIMask(const ROIMask &m);

private:
    int _width;
    int _height;
    int _block_size;
    double _threshold;
    std::vector<uint8_t> _prev_frame;
    ROIMaskPtr _roi;
};


} // namespace mediakit

#endif //S3MEDIAKIT_MOTIONDETECTOR_H