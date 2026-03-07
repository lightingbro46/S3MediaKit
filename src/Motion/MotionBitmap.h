#ifndef MOTION_MOTIONBITMAP_H
#define MOTION_MOTIONBITMAP_H

#include <cstdint>
#include <memory>
#include <vector>
#include "Codec/Transcode.h"

namespace mediakit {

#pragma pack(push,1)
struct MotionBitmap {
    int rows;
    int cols;
    // bit-packed motion map, 1 bit = 1 pixel, 0 = no motion, 1 = motion
    uint8_t bitmap[];
};
#pragma pack(pop)
using MotionBitmapPtr = std::shared_ptr<MotionBitmap>;

struct ROIMask {
    int rows;  // Number of rows in the frame
    int cols; // Number of columns in the frame
    std::vector<uint8_t> mask; // 1 byte = 1 pixel, 0 = ignore, {1, 2, 3, 4, 5} = level detect

    ROIMask(int r, int c, std::string &s);
};

using ROIMaskPtr = std::shared_ptr<ROIMask>;

class MotionBitmapHelper {
public:
    MotionBitmapHelper() = delete;
    ~MotionBitmapHelper() = delete;

    // Create a motion bitmap from a motion result
    static std::shared_ptr<MotionBitmap> createMotionBitmap(int rows, int cols, const uint8_t* result) {
        size_t bitmapSize = (rows * cols + 7) / 8;
        size_t totalSize = sizeof(MotionBitmap) + bitmapSize;
        uint8_t* buffer = new uint8_t[totalSize]();
        MotionBitmap* bmp = reinterpret_cast<MotionBitmap*>(buffer);
        bmp->rows = rows;
        bmp->cols = cols;
        if (result) {
            for (int i = 0; i < rows * cols; ++i) {
                if (result[i]) {
                    bmp->bitmap[i / 8] |= (1 << (7 - (i % 8))); // Set the corresponding bit
                }
            }
        } else {
            // If result is null, create an empty motion bitmap
            // (bitmap is already initialized to 0 by new uint8_t[totalSize]())
        }
        return std::shared_ptr<MotionBitmap>(bmp, [](MotionBitmap* p) { delete[] reinterpret_cast<uint8_t*>(p); });
    }

    // Create motion bitmap from buffer
    static std::shared_ptr<MotionBitmap> getMotionBitmap(const uint8_t* buffer, size_t size) {
        if (size < sizeof(MotionBitmap)) return nullptr;
        const MotionBitmap* bmp = reinterpret_cast<const MotionBitmap*>(buffer);
        size_t expectedSize = sizeof(MotionBitmap) + (bmp->rows * bmp->cols + 7) / 8;
        if (size < expectedSize) return nullptr; // Buffer too small
        return std::shared_ptr<MotionBitmap>(const_cast<MotionBitmap*>(bmp), [](MotionBitmap*) { /* do nothing, buffer is managed by caller */ });
    }

    // Get bit index for a given pixel coordinate
    static size_t getBitIndex(const MotionBitmap* bmp, int x, int y) {
        if (x < 0 || x >= bmp->cols || y < 0 || y >= bmp->rows) return -1; // Out of bounds
        return y * bmp->cols + x;
    }

    // Get motion value for a given pixel coordinate
    static bool getMotionValue(const MotionBitmap* bmp, int x, int y) {
        size_t bitIndex = getBitIndex(bmp, x, y);
        if (bitIndex == static_cast<size_t>(-1)) return false; // Out of bounds
        return (bmp->bitmap[bitIndex / 8] >> (7 - (bitIndex % 8))) & 0x01;
    }

    // Set motion value for a given pixel coordinate
    static void setMotionValue(MotionBitmap* bmp, int x, int y, bool motion) {
        size_t bitIndex = getBitIndex(bmp, x, y);
        if (bitIndex == static_cast<size_t>(-1)) return; // Out of bounds
        if (motion) {
            bmp->bitmap[bitIndex / 8] |= (1 << (7 - (bitIndex % 8))); // Set the bit
        } else {
            bmp->bitmap[bitIndex / 8] &= ~(1 << (7 - (bitIndex % 8))); // Clear the bit
        }
    }

    // get motion ratio
    static double getMotionRatio(const MotionBitmap* bmp) {
        size_t totalPixels = bmp->rows * bmp->cols;
        size_t motionPixels = 0;
        for (size_t i = 0; i < totalPixels; ++i) {
            if ((bmp->bitmap[i / 8] >> (7 - (i % 8))) & 0x01) {
                motionPixels++;
            }
        }
        return static_cast<double>(motionPixels) / static_cast<double>(totalPixels);
    }

    // OR operation between two motion bitmaps, result is stored in the first bitmap
    static void orMotionBitmaps(MotionBitmap* bmp1, const MotionBitmap* bmp2) {
        if (bmp1->rows != bmp2->rows || bmp1->cols != bmp2->cols) return; // Size mismatch
        size_t bitmapSize = (bmp1->rows * bmp1->cols + 7) / 8;
        for (size_t i = 0; i < bitmapSize; ++i) {
            bmp1->bitmap[i] |= bmp2->bitmap[i];
        }
    }

    // AND operation between two motion bitmaps, result is stored in the first bitmap
    static void andMotionBitmaps(MotionBitmap* bmp1, const MotionBitmap* bmp2) {
        if (bmp1->rows != bmp2->rows || bmp1->cols != bmp2->cols) return; // Size mismatch
        size_t bitmapSize = (bmp1->rows * bmp1->cols + 7) / 8;
        for (size_t i = 0; i < bitmapSize; ++i) {
            bmp1->bitmap[i] &= bmp2->bitmap[i];
        }
    }

    // intersect two motion bitmaps, return true if there is any intersection
    static bool intersectMotionBitmaps(MotionBitmap* bmp1, const MotionBitmap* bmp2) {
        if (bmp1->rows != bmp2->rows || bmp1->cols != bmp2->cols) return false; // Size mismatch
        size_t bitmapSize = (bmp1->rows * bmp1->cols + 7) / 8;
        for (size_t i = 0; i < bitmapSize; ++i) {
            if ((bmp1->bitmap[i] & bmp2->bitmap[i]) != 0) {
                return true; // Found intersection
            }
        }
        return false; // No intersection
    }
};

struct GridBoundary {
    int rows;
    int cols;

    // Y plane boundary
    std::vector<int> x;
    std::vector<int> y;

    // UV plane boundary (subsampled 2x2)
    std::vector<int> uv_x;
    std::vector<int> uv_y;

    int frame_w;
    int frame_h;
};

using GridBoundaryPtr = std::shared_ptr<GridBoundary>;

class GridBoundaryHelper {
public:
    GridBoundaryHelper() = delete;
    ~GridBoundaryHelper() = delete;

    /**
     * Precompute grid boundaries for given frame dimensions and grid size, to optimize motion detection by avoiding on-the-fly coordinate calculations
      * @param boundary Output grid boundary structure to be filled
      * @param frame_w Width of the frame
      * @param frame_h Height of the frame
      * @param grid_rows Number of rows in the grid
      * @param grid_cols Number of columns in the grid
      * @return
     */
    static void compute_grid_boundary(GridBoundary &boundary, int frame_w, int frame_h, int grid_rows, int grid_cols) {
        boundary.rows = grid_rows;
        boundary.cols = grid_cols;
        boundary.frame_w = frame_w;
        boundary.frame_h = frame_h;

        boundary.x.resize(grid_cols + 1);
        boundary.y.resize(grid_rows + 1);
        boundary.uv_x.resize(grid_cols + 1);
        boundary.uv_y.resize(grid_rows + 1);

        int uv_w = frame_w >> 1;
        int uv_h = frame_h >> 1;

        // --- Y boundary ---
        for (int c = 0; c <= grid_cols; ++c)
            boundary.x[c] = (int64_t)c * frame_w / grid_cols;

        for (int r = 0; r <= grid_rows; ++r)
            boundary.y[r] = (int64_t)r * frame_h / grid_rows;

        // --- UV boundary ---
        for (int c = 0; c <= grid_cols; ++c)
            boundary.uv_x[c] = (int64_t)c * uv_w / grid_cols;

        for (int r = 0; r <= grid_rows; ++r)
            boundary.uv_y[r] = (int64_t)r * uv_h / grid_rows;
    }
    
    /**
     * Draw motion grid on YUV420P frame, with motion cells highlighted and grid lines overlaid. This is used for debugging and visualization of motion detection results.
      * @param frame Input/output frame in YUV420P format, must be writable
      * @param mb Motion bitmap containing motion detection results
      * @param overlay_motion Whether to highlight motion cells with a semi-transparent red overlay
      * @param overlay_grid Whether to draw grid lines
      * @return
     */
    static void draw_motion_grid_yuv420p(AVFrame *frame, const MotionBitmap &mb, const GridBoundaryPtr &grid = nullptr, bool overlay_motion = true, bool overlay_grid = true) {
        if (!frame || (!overlay_motion && !overlay_grid)) return;
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) return;
        if (mb.rows <= 0 || mb.cols <= 0) return;

        av_frame_make_writable(frame);

        int W = frame->width;
        int H = frame->height;

        uint8_t* Y = frame->data[0];
        uint8_t* U = frame->data[1];
        uint8_t* V = frame->data[2];

        int Y_stride = frame->linesize[0];
        int U_stride = frame->linesize[1];
        int V_stride = frame->linesize[2];

        // Alpha blending parameters for motion overlay
        const int alpha = 120;
        const int line_alpha = 80;

        const uint8_t Y_red = 76;
        const uint8_t U_red = 84;
        const uint8_t V_red = 255;

        const uint8_t Y_white = 235;
        const uint8_t U_white = 128;
        const uint8_t V_white = 128;

        // Use precomputed boundaries if valid
        const bool use_grid =
            grid &&
            grid->rows == mb.rows && grid->cols == mb.cols &&
            grid->frame_w == W && grid->frame_h == H &&
            grid->x.size() == static_cast<size_t>(mb.cols + 1) &&
            grid->y.size() == static_cast<size_t>(mb.rows + 1) &&
            grid->uv_x.size() == static_cast<size_t>(mb.cols + 1) &&
            grid->uv_y.size() == static_cast<size_t>(mb.rows + 1);

        auto x0_of = [&](int c) { return use_grid ? grid->x[c] : (int64_t)c * W / mb.cols; };
        auto x1_of = [&](int c) { return use_grid ? grid->x[c + 1] : (int64_t)(c + 1) * W / mb.cols; };
        auto y0_of = [&](int r) { return use_grid ? grid->y[r] : (int64_t)r * H / mb.rows; };
        auto y1_of = [&](int r) { return use_grid ? grid->y[r + 1] : (int64_t)(r + 1) * H / mb.rows; };

        auto uv_x0_of = [&](int c) { return use_grid ? grid->uv_x[c] : ((int64_t)c * W / mb.cols) >> 1; };
        auto uv_x1_of = [&](int c) { return use_grid ? grid->uv_x[c + 1] : ((int64_t)(c + 1) * W / mb.cols) >> 1; };
        auto uv_y0_of = [&](int r) { return use_grid ? grid->uv_y[r] : ((int64_t)r * H / mb.rows) >> 1; };
        auto uv_y1_of = [&](int r) { return use_grid ? grid->uv_y[r + 1] : ((int64_t)(r + 1) * H / mb.rows) >> 1; };

        auto blend_u8 = [](uint8_t dst, uint8_t src, int a) -> uint8_t {
            return static_cast<uint8_t>((dst * (255 - a) + src * a) >> 8);
        };

        // ==============================
        // 1️⃣ Fill motion cells
        // ==============================
        if (overlay_motion) {
            int count = 0;
            for (int r = 0; r < mb.rows; ++r) {
                int y0 = y0_of(r);
                int y1 = y1_of(r);
                int uv_y0 = uv_y0_of(r);
                int uv_y1 = uv_y1_of(r);

                for (int c = 0; c < mb.cols; ++c) {
                    if (!MotionBitmapHelper::getMotionValue(&mb, c, r))
                        continue;
                    ++count;
                    int x0 = x0_of(c);
                    int x1 = x1_of(c);
                    int uv_x0 = uv_x0_of(c);
                    int uv_x1 = uv_x1_of(c);

                    // ---- Y plane ----
                    for (int y = y0; y < y1; ++y) {
                        uint8_t* row = Y + y * Y_stride;
                        for (int x = x0; x < x1; ++x) {
                            row[x] = blend_u8(row[x], Y_red, alpha);
                        }
                    }

                    // ---- UV plane (subsampled 2x2) ----
                    for (int y = uv_y0; y < uv_y1; ++y) {
                        uint8_t* u_row = U + y * U_stride;
                        uint8_t* v_row = V + y * V_stride;
                        for (int x = uv_x0; x < uv_x1; ++x) {
                            u_row[x] = blend_u8(u_row[x], U_red, alpha);
                            v_row[x] = blend_u8(v_row[x], V_red, alpha);
                        }
                    }
                }
            }
            TraceL << "Motion grid drawn, total motion cells: " << count;
        }
        
        // ==============================
        // 2️⃣ Draw grid lines (thickness = 1)
        // ==============================
        bool use_red_line = true; // Set to true to use red lines for grid, false to use white lines
        if (overlay_grid) {
            // Vertical lines
            for (int c = 0; c <= mb.cols; ++c) {
                int x = x0_of(c);
                if (x < 0 || x >= W) continue;

                // Y plane
                for (int y = 0; y < H; ++y)
                    Y[y * Y_stride + x] = blend_u8(Y[y * Y_stride + x], use_red_line ? Y_red : Y_white, line_alpha);

                // UV plane
                int uv_x = use_grid ? grid->uv_x[c] : (x >> 1);
                if (uv_x >= 0 && uv_x < (W >> 1)) {
                    for (int y = 0; y < (H >> 1); ++y) {
                        U[y * U_stride + uv_x] = blend_u8(U[y * U_stride + uv_x], use_red_line ? U_red : U_white, line_alpha);
                        V[y * V_stride + uv_x] = blend_u8(V[y * V_stride + uv_x], use_red_line ? V_red : V_white, line_alpha);
                    }
                }
            }

            // Horizontal lines
            for (int r = 0; r <= mb.rows; ++r) {
                int y = y0_of(r);
                if (y < 0 || y >= H) continue;

                // Y plane
                for (int x = 0; x < W; ++x)
                    Y[y * Y_stride + x] = blend_u8(Y[y * Y_stride + x], use_red_line ? Y_red : Y_white, line_alpha);

                // UV plane
                int uv_y = use_grid ? grid->uv_y[r] : (y >> 1);
                if (uv_y >= 0 && uv_y < (H >> 1)) {
                    for (int x = 0; x < (W >> 1); ++x) {
                        U[uv_y * U_stride + x] = blend_u8(U[uv_y * U_stride + x], use_red_line ? U_red : U_white, line_alpha);
                        V[uv_y * V_stride + x] = blend_u8(V[uv_y * V_stride + x], use_red_line ? V_red : V_white, line_alpha);
                    }
                }
            }
        }
    }

    static void draw_roi_border_yuv420p(AVFrame *frame, const ROIMask &roi, const GridBoundaryPtr &grid = nullptr, bool fill_border = false, bool draw_level = false) {
        if (!frame) return;
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) return;
        if (roi.rows <= 0 || roi.cols <= 0) return;

        av_frame_make_writable(frame);

        const int W = frame->width;
        const int H = frame->height;

        uint8_t* Y = frame->data[0];
        uint8_t* U = frame->data[1];
        uint8_t* V = frame->data[2];

        const int Y_stride = frame->linesize[0];
        const int U_stride = frame->linesize[1];
        const int V_stride = frame->linesize[2];

        const uint8_t Y_white = 235;
        const uint8_t U_white = 128;
        const uint8_t V_white = 128;

        const bool use_grid =
            grid &&
            grid->rows == roi.rows && grid->cols == roi.cols &&
            grid->frame_w == W && grid->frame_h == H &&
            grid->x.size() == static_cast<size_t>(roi.cols + 1) &&
            grid->y.size() == static_cast<size_t>(roi.rows + 1) &&
            grid->uv_x.size() == static_cast<size_t>(roi.cols + 1) &&
            grid->uv_y.size() == static_cast<size_t>(roi.rows + 1);

        auto x_of = [&](int c) { return use_grid ? grid->x[c] : static_cast<int>((int64_t)c * W / roi.cols); };
        auto y_of = [&](int r) { return use_grid ? grid->y[r] : static_cast<int>((int64_t)r * H / roi.rows); };
        // auto uv_x_of = [&](int c) { return use_grid ? grid->uv_x[c] : (x_of(c) >> 1); };
        // auto uv_y_of = [&](int r) { return use_grid ? grid->uv_y[r] : (y_of(r) >> 1); };

        auto at = [&](int r, int c) -> uint8_t {
            if (r < 0 || r >= roi.rows || c < 0 || c >= roi.cols) return 0;
            return roi.mask[r * roi.cols + c];
        };

        auto draw_vline = [&](int x, int y0, int y1) {
            if (x < 0 || x >= W) return;
            y0 = std::max(0, y0); y1 = std::min(H, y1);
            for (int y = y0; y < y1; ++y) Y[y * Y_stride + x] = Y_white;

            const int uvx = x >> 1;
            if (uvx < 0 || uvx >= (W >> 1)) return;
            const int uvy0 = std::max(0, y0 >> 1);
            const int uvy1 = std::min(H >> 1, (y1 + 1) >> 1);
            for (int y = uvy0; y < uvy1; ++y) {
                U[y * U_stride + uvx] = U_white;
                V[y * V_stride + uvx] = V_white;
            }
        };

        auto draw_hline = [&](int y, int x0, int x1) {
            if (y < 0 || y >= H) return;
            x0 = std::max(0, x0); x1 = std::min(W, x1);
            uint8_t* yrow = Y + y * Y_stride;
            for (int x = x0; x < x1; ++x) yrow[x] = Y_white;

            const int uvy = y >> 1;
            if (uvy < 0 || uvy >= (H >> 1)) return;
            const int uvx0 = std::max(0, x0 >> 1);
            const int uvx1 = std::min(W >> 1, (x1 + 1) >> 1);
            uint8_t* urow = U + uvy * U_stride;
            uint8_t* vrow = V + uvy * V_stride;
            for (int x = uvx0; x < uvx1; ++x) {
                urow[x] = U_white;
                vrow[x] = V_white;
            }
        };

        for (int r = 0; r < roi.rows; ++r) {
            const int y0 = y_of(r);
            const int y1 = y_of(r + 1);

            for (int c = 0; c < roi.cols; ++c) {
                if (at(r, c) == 0) continue;

                const int x0 = x_of(c);
                const int x1 = x_of(c + 1);

                // Chỉ vẽ cạnh tiếp giáp vùng non-ROI để giảm overdraw
                if (at(r - 1, c) == 0) draw_hline(y0, x0, x1);      // top
                if (at(r + 1, c) == 0) draw_hline(y1 - 1, x0, x1);  // bottom
                if (at(r, c - 1) == 0) draw_vline(x0, y0, y1);      // left
                if (at(r, c + 1) == 0) draw_vline(x1 - 1, y0, y1);  // right
            }
        }

        if (draw_level) {
            // 3x5 font for digits 0..9 (bit2..bit0)
            static const uint8_t DIGITS[10][5] = {
                {0b111,0b101,0b101,0b101,0b111}, // 0
                {0b010,0b110,0b010,0b010,0b111}, // 1
                {0b111,0b001,0b111,0b100,0b111}, // 2
                {0b111,0b001,0b111,0b001,0b111}, // 3
                {0b101,0b101,0b111,0b001,0b001}, // 4
                {0b111,0b100,0b111,0b001,0b111}, // 5
                {0b111,0b100,0b111,0b101,0b111}, // 6
                {0b111,0b001,0b010,0b010,0b010}, // 7
                {0b111,0b101,0b111,0b101,0b111}, // 8
                {0b111,0b101,0b111,0b001,0b111}  // 9
            };

            auto draw_digit = [&](int x, int y, int d, uint8_t val) {
                if (d < 0 || d > 9) return;
                for (int r = 0; r < 5; ++r) {
                    const int yy = y + r;
                    if (yy < 0 || yy >= H) continue;
                    uint8_t *row = Y + yy * Y_stride;
                    for (int c = 0; c < 3; ++c) {
                        const int xx = x + c;
                        if (xx < 0 || xx >= W) continue;
                        if ((DIGITS[d][r] >> (2 - c)) & 0x1) row[xx] = val;
                    }
                }
            };

            std::vector<uint8_t> visited(static_cast<size_t>(roi.rows * roi.cols), 0);

            for (int sr = 0; sr < roi.rows; ++sr) {
                for (int sc = 0; sc < roi.cols; ++sc) {
                    const int sidx = sr * roi.cols + sc;
                    const uint8_t level = roi.mask[sidx];
                    if (level == 0 || visited[sidx]) continue;

                    int min_r = sr, min_c = sc;
                    std::vector<int> stack;
                    stack.push_back(sidx);
                    visited[sidx] = 1;

                    while (!stack.empty()) {
                        const int idx = stack.back();
                        stack.pop_back();

                        const int r = idx / roi.cols;
                        const int c = idx % roi.cols;

                        if (r < min_r || (r == min_r && c < min_c)) {
                            min_r = r;
                            min_c = c;
                        }

                        const int dr[4] = {-1, 1, 0, 0};
                        const int dc[4] = {0, 0, -1, 1};
                        for (int k = 0; k < 4; ++k) {
                            const int nr = r + dr[k];
                            const int nc = c + dc[k];
                            if (nr < 0 || nr >= roi.rows || nc < 0 || nc >= roi.cols) continue;
                            const int nidx = nr * roi.cols + nc;
                            if (visited[nidx]) continue;
                            if (roi.mask[nidx] != level) continue; // same level component
                            visited[nidx] = 1;
                            stack.push_back(nidx);
                        }
                    }

                    const int tx = x_of(min_c) + 2;
                    const int ty = y_of(min_r) + 2;

                    if (level >= 10) {
                        draw_digit(tx, ty, (level / 10) % 10, 235);
                        draw_digit(tx + 4, ty, level % 10, 235);
                    } else {
                        draw_digit(tx, ty, level, 235);
                    }
                }
            }
        }
    }

};

} // namespace mediakit

#endif // MOTION_MOTIONBITMAP_H