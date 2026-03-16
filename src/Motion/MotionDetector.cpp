#ifdef ENABLE_MOTION

#include <cmath>
#include <vector>
#include "Common/config.h"
#include "MotionDetector.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

static vector<double> g_sensitivity; // Sensitivity thresholds for different ROI levels, indexed by (level - 1)
static onceToken token([]() {
    GET_CONFIG(string, sensitivity_str, Motion::kSensitivity);
    for (auto &th : split(sensitivity_str, ",")) {
        trim(th);
        if (!th.empty()) {
            g_sensitivity.emplace_back(stod(th));
        }
    }
});

ROIMask::ROIMask(int r, int c, std::string &s) : rows(r), cols(c), mask(r * c, 0) {
    if (s.empty()) {
        GET_CONFIG(int, roi_level, Motion::kROIDefaultLevel);
        DebugL << "No ROI mask provided, motion detection will be performed on the entire frame with default level: " << roi_level;
        s = string(MOTION_GRID_ROWS * MOTION_GRID_COLS, static_cast<char>('0' + roi_level)); // Default to full frame detection with size 440x320
    }
    if (s.size() != static_cast<size_t>(r * c)) {
        WarnL << "Invalid ROI mask string, size does not match [rows * cols]";
        return;
    }
    DebugL << "ROI mask created with rows: " << r << ", cols: " << c << ", mask string size: " << s.size();
    for (size_t i = 0; i < s.size(); ++i) {
        auto &ch = s[i];
        if (ch < '0' || ch > '5') {
            WarnL << "Invalid character in ROI mask string, only '0'-'5' are allowed";
            continue;
        }
        mask[i] = static_cast<uint8_t>(s[i] - '0');
    }
}

////////////////////////////////////MotionDetector////////////////////////////////

MotionDetector::MotionDetector(int width, int height, const ROIMaskPtr &roi_mask) 
    : _width(width), _height(height), _roi(std::move(roi_mask)) {
    CHECK(width > 0 && height > 0 && roi_mask != nullptr, "Invalid frame dimensions");
    _prev_frame.resize(_width * _height);
    _grid_boundary = std::make_shared<GridBoundary>();
    GridBoundaryHelper::compute_grid_boundary(*_grid_boundary, _width, _height, _roi->rows, _roi->cols);
}

MotionDetector::~MotionDetector() {
    _prev_frame.clear();
    _roi.reset();
};

bool MotionDetector::inputFrame(const uint8_t* data, int linesize, uint64_t pts_ms) {
    if (!data || !_roi || _roi->rows <= 0 || _roi->cols <= 0 || linesize < _width) {
        WarnL << "Invalid input frame or ROI";
        return false;
    }

    if (g_sensitivity.empty()) {
        WarnL << "Sensitivity config is empty";
        return false;
    }

    // Use precomputed grid boundaries to iterate over blocks, which is more efficient than calculating pixel coordinates on the fly
    const auto &x_bounds = _grid_boundary->x;
    const auto &y_bounds = _grid_boundary->y;
    if (x_bounds.size() != static_cast<size_t>(_roi->cols + 1) ||
        y_bounds.size() != static_cast<size_t>(_roi->rows + 1)) {
        WarnL << "Invalid grid boundary size";
        return false;
    }

    auto result = MotionBitmapHelper::createMotionBitmap(_roi->rows, _roi->cols, nullptr); // Create empty motion bitmap
    int motion_blocks = 0;
    int active_blocks = 0;

    for (int by = 0; by < _roi->rows; ++by) {
        const int y0 = y_bounds[by];
        const int y1 = y_bounds[by + 1];
        
        for (int bx = 0; bx < _roi->cols; ++bx) {
            const uint8_t level = _roi->mask[by * _roi->cols + bx];
            if (level == 0) {
                continue; // Ignore this block
            }
            if (level > g_sensitivity.size()) {
                WarnL << "Invalid ROI level: " << level << ", exceeds sensitivity configuration";
                continue;
            }

            const int x0 = x_bounds[bx];
            const int x1 = x_bounds[bx + 1];
            const int block_w = x1 - x0;
            const int block_h = y1 - y0;
            const int area = block_w * block_h;
            if (area <= 0) {
                WarnL << "Invalid block area: " << area << " for block (" << bx << ", " << by << ")";
                continue;
            }
            
            ++active_blocks;

            uint64_t sum_diff = 0;
            for (int y = y0; y < y1; ++y) {
                const int row_src = y * linesize;
                const int row_prev = y * _width;
                for (int x = x0; x < x1; ++x) {
                    const uint8_t curr_pixel = data[row_src + x];
                    const uint8_t prev_pixel = _prev_frame[row_prev + x];
                    sum_diff += std::abs(static_cast<int>(curr_pixel) - static_cast<int>(prev_pixel));
                }
            }
            // Compare block difference with division: sum_diff / (area * 255.0) > threshold
            // <=> sum_diff > threshold * area * 255.0
            const double threshold = g_sensitivity[level - 1];
            const double threshold_scaled = threshold * static_cast<double>(area) * 255.0;

            if (static_cast<double>(sum_diff) > threshold_scaled) {
                ++motion_blocks;
                MotionBitmapHelper::setMotionValue(result.get(), bx, by, true);
            }
        }
    }

    // Update previous frame buffer (row-wise copy)
    for (int y = 0; y < _height; ++y) {
        std::memcpy(&_prev_frame[y * _width], data + y * linesize, static_cast<size_t>(_width));
    }

    // Skip motion event on first frame: _prev_frame was uninitialized (all zeros),
    // so any comparison would produce false positives.
    if (_first_frame) {
        _first_frame = false;
        return true;
    }

    const bool motion_detected = (motion_blocks > 0);
    if (_on_result) {
        auto stamp_ms = getCurrentMillisecond(true);
        // DebugL << "Motion blocks: " << motion_blocks << "/" << active_blocks
        //        << ", motion_detected: " << motion_detected
        //        << ", stamp_ms: " << stamp_ms;
        _on_result(motion_detected, stamp_ms, result);
    }
    return true;
}

} // namespace mediakit 

#endif // ENABLE_MOTION
