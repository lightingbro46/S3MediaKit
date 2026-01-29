#include "MotionDetector.h"
#include <cmath>

namespace mediakit {

static int pointInPolygon(int x, int y, const Polygon &p) {
    return 1; // Placeholder: Implement point-in-polygon algorithm if needed
}

static ROIMask toMask(const Polygon &p, int w, int h) {
    ROIMask mask(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            mask.mask[y*w + x] = pointInPolygon(x, y, p);

    return mask;
}

MotionDetector::MotionDetector(int width, int height, int block_size, double threshold)
    : _width(width), _height(height), _block_size(block_size), _threshold(threshold) {
    _prev_frame.resize(width * height);
    _roi = std::make_shared<ROIMask>(width, height);
}

MotionDetector::~MotionDetector() {};

MotionResult MotionDetector::processFrame(const uint8_t* data, int linesize, uint64_t pts_ms) {
    MotionResult result(_width, _height);
    result.roi = _roi;
    result.pts_ms = pts_ms;

    int blocks_x = _width / _block_size;
    int blocks_y = _height / _block_size;
    int total_blocks = blocks_x * blocks_y;
    int motion_blocks = 0;

    for (int by = 0; by < blocks_y; ++by) {
        for (int bx = 0; bx < blocks_x; ++bx) {
            double block_diff = 0.0;

            for (int y = 0; y < _block_size; ++y) {
                for (int x = 0; x < _block_size; ++x) {
                    int frame_y = by * _block_size + y;
                    int frame_x = bx * _block_size + x;
                    int index = frame_y * linesize + frame_x;
                    if (result.roi && !result.roi->mask[index]) continue; // Skip if outside ROI

                    uint8_t curr_pixel = data[index];
                    uint8_t prev_pixel = _prev_frame[frame_y * _width + frame_x];
                    block_diff += std::abs(static_cast<int>(curr_pixel) - static_cast<int>(prev_pixel));
                }
            }

            block_diff /= (_block_size * _block_size * 255.0); // Normalize to [0,1]
            if (block_diff > _threshold) {
                motion_blocks++;
                // Mark motion in motion_map
                for (int y = 0; y < _block_size; ++y) {
                    for (int x = 0; x < _block_size; ++x) {
                        int frame_y = by * _block_size + y;
                        int frame_x = bx * _block_size + x;
                        int index = frame_y * _width + frame_x;
                        result.motion_map[index] = 1;
                    }
                }
            }
        }
    }

    double motion_ratio = static_cast<double>(motion_blocks) / static_cast<double>(total_blocks);
    result.ratio = motion_ratio;
    result.motion_detected = motion_ratio > 0.0;

    for (int y = 0; y < _height; ++y) {
        for (int x = 0; x < _width; ++x) {
            _prev_frame[y * _width + x] = data[y * linesize + x];
        }
    }

    return result;
}

void MotionDetector::setPolygonMask(const Polygon &p) {
    auto roi = toMask(p, _width, _height);
    setROIMask(roi);
}

void MotionDetector::setROIMask(const ROIMask &m) {
    _roi = std::make_shared<ROIMask>(m);
}

} // namespace mediakit 

