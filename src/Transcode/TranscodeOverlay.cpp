#if defined(ENABLE_FFMPEG)

#include "TranscodeOverlay.h"
#include "Util/File.h"
#include "Util/logger.h"

#include <sstream>
#include <stdint.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#if defined(_WIN32)
#include <process.h>
#define MK_GETPID _getpid
#else
#include <unistd.h>
#define MK_GETPID getpid
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/pixdesc.h"
#ifdef __cplusplus
}
#endif

using namespace std;
using namespace toolkit;

namespace mediakit {

static string escape_movie_path(const string &path) {
    // avfilter option escaping: backslash, single-quote and colon are special.
    string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '\\' || c == ':' || c == '\'') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

TranscodeOverlay::TranscodeOverlay(std::string image_path, int x, int y)
    : _image_path(std::move(image_path)), _x(x), _y(y) {
    if (_image_path.empty()) {
        WarnL << "TranscodeOverlay: empty image path, overlay disabled";
        return;
    }
    _overlay_component_count = 1;
    if (!File::fileExist(_image_path)) {
        WarnL << "TranscodeOverlay: image not found: " << _image_path << ", overlay disabled";
        return;
    }
    _valid = true;
}

TranscodeOverlay::TranscodeOverlay(const std::vector<OverlayComponent> &components,
                                   const OverlayBuildOptions &options) {
    if (components.empty() && options.privacy_masks.empty() && options.prebuilt_svg_path.empty()) {
        WarnL << "TranscodeOverlay: no watermark components or privacy masks, overlay disabled";
        return;
    }

    if (!options.prebuilt_svg_path.empty()) {
        _image_path = options.prebuilt_svg_path;
        _overlay_component_count = 1;
        _privacy_mask_count = options.privacy_masks.size();
        _privacy_masks = options.privacy_masks;
        _overlay_canvas_width = std::max(1, options.canvas_width);
        _overlay_canvas_height = std::max(1, options.canvas_height);
        if (!File::fileExist(_image_path) || File::fileSize(_image_path) == 0) {
            WarnL << "TranscodeOverlay: prebuilt watermark SVG not found: " << _image_path;
            _image_path.clear();
            return;
        }
        _valid = true;
        return;
    }

    std::ostringstream path;
    path << "/tmp/s3mediakit_overlay_" << static_cast<long long>(MK_GETPID())
         << "_" << reinterpret_cast<uintptr_t>(this) << ".svg";
    _generated_image_path = path.str();
    _image_path = _generated_image_path;
    _overlay_component_count = components.size();
    _privacy_mask_count = options.privacy_masks.size();
    _privacy_masks = options.privacy_masks;
    _overlay_canvas_width = std::max(1, options.canvas_width);
    _overlay_canvas_height = std::max(1, options.canvas_height);

    OverlayBuildOptions watermark_options = options;
    // Privacy masks are applied directly to YUV frames. Keep only watermark
    // components in the SVG consumed by the remaining watermark graph.
    watermark_options.privacy_masks.clear();
    const std::string svg = OverlayPrivacyUtils::buildSvg(components, watermark_options);
    if (!File::saveFile(svg, _generated_image_path)) {
        WarnL << "TranscodeOverlay: cannot save generated SVG: " << _generated_image_path;
        _generated_image_path.clear();
        _image_path.clear();
        return;
    }

    _valid = true;
}

namespace {
struct YuvColor {
    uint8_t y;
    uint8_t u;
    uint8_t v;
};

YuvColor parseYuvColor(const std::string &value) {
    unsigned int rgb = 0;
    if (value.size() == 7 && value[0] == '#') {
        rgb = static_cast<unsigned int>(std::strtoul(value.substr(1).c_str(), nullptr, 16));
    }
    const double r = (rgb >> 16) & 0xff;
    const double g = (rgb >> 8) & 0xff;
    const double b = rgb & 0xff;
    const double y = 0.299 * r + 0.587 * g + 0.114 * b;
    return {static_cast<uint8_t>(std::max(0.0, std::min(255.0, y))),
            static_cast<uint8_t>(std::max(0.0, std::min(255.0, 128.0 - 0.169 * r - 0.331 * g + 0.5 * b))),
            static_cast<uint8_t>(std::max(0.0, std::min(255.0, 128.0 + 0.5 * r - 0.419 * g - 0.081 * b)))};
}

uint8_t blendByte(uint8_t source, uint8_t replacement, uint8_t alpha) {
    if (alpha == 255) {
        return replacement;
    }
    return static_cast<uint8_t>((source * (255 - alpha) + replacement * alpha + 127) / 255);
}

void boxBlurHorizontal(const std::vector<uint8_t> &src, std::vector<uint8_t> &dst,
                       int width, int height, int radius) {
    dst.resize(src.size());
    for (int y = 0; y < height; ++y) {
        const int row = y * width;
        int sum = 0;
        int left = 0;
        int right = std::min(width - 1, radius);
        for (int x = left; x <= right; ++x) {
            sum += src[row + x];
        }
        for (int x = 0; x < width; ++x) {
            dst[row + x] = static_cast<uint8_t>(sum / (right - left + 1));
            const int next_left = std::max(0, x + 1 - radius);
            const int next_right = std::min(width - 1, x + 1 + radius);
            while (left < next_left) sum -= src[row + left++];
            while (right < next_right) sum += src[row + ++right];
        }
    }
}

void boxBlurVertical(const std::vector<uint8_t> &src, std::vector<uint8_t> &dst,
                     int width, int height, int radius) {
    dst.resize(src.size());
    for (int x = 0; x < width; ++x) {
        int sum = 0;
        int top = 0;
        int bottom = std::min(height - 1, radius);
        for (int y = top; y <= bottom; ++y) {
            sum += src[y * width + x];
        }
        for (int y = 0; y < height; ++y) {
            dst[y * width + x] = static_cast<uint8_t>(sum / (bottom - top + 1));
            const int next_top = std::max(0, y + 1 - radius);
            const int next_bottom = std::min(height - 1, y + 1 + radius);
            while (top < next_top) sum -= src[top++ * width + x];
            while (bottom < next_bottom) sum += src[++bottom * width + x];
        }
    }
}
}

bool TranscodeOverlay::buildPrivacyMaskCache(int width, int height) {
    _cached_privacy_masks.clear();
    if (_privacy_masks.empty()) {
        return true;
    }
    for (size_t i = 0; i < _privacy_masks.size(); ++i) {
        const PrivacyMaskRegion &mask = _privacy_masks[i];
        if (mask.points.size() < 3) {
            continue;
        }
        CachedPrivacyMask cached;
        cached.config = mask;
        std::vector<std::vector<std::pair<int, int> > > luma_spans(height);
        for (size_t p = 0; p < mask.points.size(); ++p) {
            const double x = mask.points[p].first * width / _overlay_canvas_width;
            const double y = mask.points[p].second * height / _overlay_canvas_height;
            if (p == 0) {
                cached.min_x = cached.max_x = static_cast<int>(x);
                cached.min_y = cached.max_y = static_cast<int>(y);
            } else {
                cached.min_x = std::min(cached.min_x, static_cast<int>(x));
                cached.max_x = std::max(cached.max_x, static_cast<int>(x));
                cached.min_y = std::min(cached.min_y, static_cast<int>(y));
                cached.max_y = std::max(cached.max_y, static_cast<int>(y));
            }
        }
        cached.min_x = std::max(0, std::min(width - 1, cached.min_x));
        cached.max_x = std::max(0, std::min(width - 1, cached.max_x));
        cached.min_y = std::max(0, std::min(height - 1, cached.min_y));
        cached.max_y = std::max(0, std::min(height - 1, cached.max_y));
        for (int y = cached.min_y; y <= cached.max_y; ++y) {
            std::vector<double> intersections;
            for (size_t p = 0; p < mask.points.size(); ++p) {
                const std::pair<double, double> a = mask.points[p];
                const std::pair<double, double> b = mask.points[(p + 1) % mask.points.size()];
                const double ax = a.first * width / _overlay_canvas_width;
                const double ay = a.second * height / _overlay_canvas_height;
                const double bx = b.first * width / _overlay_canvas_width;
                const double by = b.second * height / _overlay_canvas_height;
                if ((ay <= y && by > y) || (by <= y && ay > y)) {
                    intersections.push_back(ax + (y - ay) * (bx - ax) / (by - ay));
                }
            }
            std::sort(intersections.begin(), intersections.end());
            for (size_t p = 0; p + 1 < intersections.size(); p += 2) {
                const int left = std::max(0, static_cast<int>(std::ceil(intersections[p])));
                const int right = std::min(width - 1, static_cast<int>(std::floor(intersections[p + 1])));
                if (left <= right) luma_spans[y].push_back(std::make_pair(left, right));
            }
        }

        const YuvColor color = parseYuvColor(mask.color);
        cached.fill[0] = color.y;
        cached.fill[1] = color.u;
        cached.fill[2] = color.v;
        cached.alpha = static_cast<uint8_t>(std::max(0.0, std::min(255.0,
            std::max(0.0, std::min(1.0, mask.opacity)) * 255.0 + 0.5)));

        const int plane_widths[3] = {width, (width + 1) / 2, (width + 1) / 2};
        const int plane_heights[3] = {height, (height + 1) / 2, (height + 1) / 2};
        for (int plane = 0; plane < 3; ++plane) {
            const int scale = plane == 0 ? 1 : 2;
            CachedPrivacyMask::PlaneCache &cache = cached.planes[plane];
            cache.min_x = cached.min_x / scale;
            cache.max_x = std::min(plane_widths[plane] - 1, cached.max_x / scale);
            cache.min_y = cached.min_y / scale;
            cache.max_y = std::min(plane_heights[plane] - 1, cached.max_y / scale);
            if (cache.min_x > cache.max_x || cache.min_y > cache.max_y) {
                continue;
            }
            cache.spans.resize(cache.max_y - cache.min_y + 1);
            for (int y = cache.min_y; y <= cache.max_y; ++y) {
                const int source_y = std::min(height - 1, y * scale);
                std::vector<std::pair<int, int> > &row = cache.spans[y - cache.min_y];
                for (size_t s = 0; s < luma_spans[source_y].size(); ++s) {
                    const int left = std::max(cache.min_x, luma_spans[source_y][s].first / scale);
                    const int right = std::min(cache.max_x, luma_spans[source_y][s].second / scale);
                    if (left <= right) {
                        row.push_back(std::make_pair(left, right));
                    }
                }
            }
        }
        _cached_privacy_masks.emplace_back(std::move(cached));
    }
    return true;
}

FFmpegFrame::Ptr TranscodeOverlay::applyPrivacyMasks(const FFmpegFrame::Ptr &frame) {
    if (_privacy_masks.empty()) return frame;
    FFmpegFrame::Ptr yuv_frame = frame;
    AVFrame *input = yuv_frame->get();
    if (input->format != AV_PIX_FMT_YUV420P) {
        if (!_privacy_sws) {
            _privacy_sws = std::make_shared<FFmpegSws>(AV_PIX_FMT_YUV420P, input->width, input->height);
        }
        yuv_frame = _privacy_sws->inputFrame(frame);
        if (!yuv_frame || !yuv_frame->get()) {
            WarnL << "TranscodeOverlay: cannot convert privacy frame to yuv420p from "
                  << av_get_pix_fmt_name(static_cast<AVPixelFormat>(input->format));
            return frame;
        }
        input = yuv_frame->get();
    }
    if (_cached_privacy_masks.empty() || _privacy_width != input->width || _privacy_height != input->height) {
        _privacy_width = input->width;
        _privacy_height = input->height;
        buildPrivacyMaskCache(_privacy_width, _privacy_height);
    }
    FFmpegFrame::Ptr output = yuv_frame->clone();
    if (!output) return frame;
    AVFrame *dst = output->get();
    for (size_t m = 0; m < _cached_privacy_masks.size(); ++m) {
        CachedPrivacyMask &mask = _cached_privacy_masks[m];
        for (int plane = 0; plane < 3; ++plane) {
            CachedPrivacyMask::PlaneCache &cache = mask.planes[plane];
            const int min_x = cache.min_x;
            const int max_x = cache.max_x;
            const int min_y = cache.min_y;
            const int max_y = cache.max_y;
            if (min_x > max_x || min_y > max_y) continue;
            const int rw = max_x - min_x + 1;
            const int rh = max_y - min_y + 1;

            if (mask.config.mask_type == PrivacyMaskRegion::BLUR) {
                // Blur a low-resolution ROI and upscale by nearest sampling.
                // In source-pixel space both luma and chroma use ~4x reduction.
                const int downscale = plane == 0 ? 4 : 2;
                const int low_w = (rw + downscale - 1) / downscale;
                const int low_h = (rh + downscale - 1) / downscale;
                cache.work.resize(low_w * low_h);
                for (int ly = 0; ly < low_h; ++ly) {
                    const int sy = std::min(max_y, min_y + ly * downscale + downscale / 2);
                    const uint8_t *src = input->data[plane] + sy * input->linesize[plane];
                    for (int lx = 0; lx < low_w; ++lx) {
                        const int sx = std::min(max_x, min_x + lx * downscale + downscale / 2);
                        cache.work[ly * low_w + lx] = src[sx];
                    }
                }
                boxBlurHorizontal(cache.work, cache.temp, low_w, low_h, 2);
                boxBlurVertical(cache.temp, cache.work, low_w, low_h, 2);
            } else if (mask.config.mask_type == PrivacyMaskRegion::PIXELATE) {
                const int block = plane == 0 ? 12 : 6;
                const int blocks_x = (rw + block - 1) / block;
                const int blocks_y = (rh + block - 1) / block;
                cache.work.resize(blocks_x * blocks_y);
                for (int by = 0; by < blocks_y; ++by) {
                    const int y0 = min_y + by * block;
                    const int y1 = std::min(max_y + 1, y0 + block);
                    for (int bx = 0; bx < blocks_x; ++bx) {
                        const int x0 = min_x + bx * block;
                        const int x1 = std::min(max_x + 1, x0 + block);
                        int sum = 0;
                        int count = 0;
                        for (int y = y0; y < y1; ++y) {
                            const uint8_t *src = input->data[plane] + y * input->linesize[plane];
                            for (int x = x0; x < x1; ++x) {
                                sum += src[x];
                                ++count;
                            }
                        }
                        cache.work[by * blocks_x + bx] = static_cast<uint8_t>(sum / std::max(1, count));
                    }
                }
            }

            for (int y = min_y; y <= max_y; ++y) {
                uint8_t *out = dst->data[plane] + y * dst->linesize[plane];
                const std::vector<std::pair<int, int> > &row = cache.spans[y - min_y];
                for (size_t s = 0; s < row.size(); ++s) {
                    const int left = row[s].first;
                    const int right = row[s].second;
                    if (mask.config.mask_type == PrivacyMaskRegion::SOLID) {
                        if (mask.alpha == 255) {
                            std::fill(out + left, out + right + 1, mask.fill[plane]);
                        } else {
                            for (int x = left; x <= right; ++x) {
                                out[x] = blendByte(out[x], mask.fill[plane], mask.alpha);
                            }
                        }
                    } else if (mask.config.mask_type == PrivacyMaskRegion::BLUR) {
                        const int downscale = plane == 0 ? 4 : 2;
                        const int low_w = (rw + downscale - 1) / downscale;
                        const int ly = std::min((rh - 1) / downscale, (y - min_y) / downscale);
                        for (int x = left; x <= right; ++x) {
                            const int lx = std::min(low_w - 1, (x - min_x) / downscale);
                            out[x] = blendByte(out[x], cache.work[ly * low_w + lx], mask.alpha);
                        }
                    } else {
                        const int block = plane == 0 ? 12 : 6;
                        const int blocks_x = (rw + block - 1) / block;
                        const int by = (y - min_y) / block;
                        for (int x = left; x <= right; ++x) {
                            const int bx = (x - min_x) / block;
                            out[x] = blendByte(out[x], cache.work[by * blocks_x + bx], mask.alpha);
                        }
                    }
                }
            }
        }
    }
    return output;
}

TranscodeOverlay::~TranscodeOverlay() {
    freeGraph();
    if (!_generated_image_path.empty()) {
        File::delete_file(_generated_image_path, false, false);
    }
}

void TranscodeOverlay::freeGraph() {
    if (_graph) {
        avfilter_graph_free(&_graph);
        _graph = nullptr;
    }
    _buffersrc_ctx = nullptr;
    _buffersink_ctx = nullptr;
}

bool TranscodeOverlay::buildGraph(const FFmpegFrame::Ptr &frame) {
    freeGraph();

    auto f = frame->get();
    _src_width = f->width;
    _src_height = f->height;
    _src_format = f->format;

    _graph = avfilter_graph_alloc();
    if (!_graph) {
        WarnL << "TranscodeOverlay: avfilter_graph_alloc failed";
        return false;
    }

    char src_args[512];
    AVRational sar = f->sample_aspect_ratio;
    if (sar.num == 0 || sar.den == 0) {
        sar = AVRational{ 1, 1 };
    }
    snprintf(src_args, sizeof(src_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1000:pixel_aspect=%d/%d",
             _src_width, _src_height, _src_format, sar.num, sar.den);

    int ret = avfilter_graph_create_filter(&_buffersrc_ctx, avfilter_get_by_name("buffer"),
                                            "in", src_args, nullptr, _graph);
    if (ret < 0) {
        WarnL << "TranscodeOverlay: create buffersrc failed: " << ret;
        freeGraph();
        return false;
    }

    ret = avfilter_graph_create_filter(&_buffersink_ctx, avfilter_get_by_name("buffersink"),
                                       "out", nullptr, nullptr, _graph);
    if (ret < 0) {
        WarnL << "TranscodeOverlay: create buffersink failed: " << ret;
        freeGraph();
        return false;
    }

    std::ostringstream graph;
    // The SVG is authored in the camera overlay canvas. Scale it to exactly the decoded frame
    // dimensions so canvas coordinates map across the complete video, including aspect-ratio changes.
    // The watermark is static. Let movie decode exactly one SVG/image frame,
    // scale it once, then let overlay repeat that last frame. Feeding the
    // movie through loop caused SVG rasterization/scale work on every video
    // frame and dominated the transcode cost.
    graph << "movie=" << escape_movie_path(_image_path) << ",scale=" << _src_width << ":"
          << _src_height << ":flags=lanczos[wm];[in]format=yuv420p[base];[base][wm]overlay=x="
          << _x << ":y=" << _y << ":eof_action=repeat:repeatlast=1:shortest=0,format=yuv420p[out]";
    const std::string desc = graph.str();
    DebugL << "TranscodeOverlay filter graph: " << desc;

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        freeGraph();
        return false;
    }
    // "in" label consumes frames produced by buffersrc.
    outputs->name = av_strdup("in");
    outputs->filter_ctx = _buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    // "out" label feeds the buffersink.
    inputs->name = av_strdup("out");
    inputs->filter_ctx = _buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    ret = avfilter_graph_parse_ptr(_graph, desc.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    if (ret < 0) {
        WarnL << "TranscodeOverlay: graph parse failed: " << ret << " (" << desc << ")";
        freeGraph();
        return false;
    }

    ret = avfilter_graph_config(_graph, nullptr);
    if (ret < 0) {
        WarnL << "TranscodeOverlay: graph config failed: " << ret;
        freeGraph();
        return false;
    }

    DebugL << "TranscodeOverlay ready: " << _image_path << " at (" << _x << "," << _y
          << ") canvas " << _src_width << "x" << _src_height
          << ", watermark components=" << _overlay_component_count
          << ", privacy masks=" << _privacy_mask_count;
    return true;
}

FFmpegFrame::Ptr TranscodeOverlay::inputFrame(const FFmpegFrame::Ptr &frame) {
    if (!_valid || !frame || !frame->get()) {
        return frame;
    }
    FFmpegFrame::Ptr privacy_frame = applyPrivacyMasks(frame);
    if (_overlay_component_count == 0) {
        return privacy_frame;
    }
    auto f = privacy_frame->get();
    if (!_graph || f->width != _src_width || f->height != _src_height || f->format != _src_format) {
        if (!buildGraph(privacy_frame)) {
            // Disable overlay permanently on hard failure to avoid per-frame spam.
            _valid = false;
            return frame;
        }
    }

    if (av_buffersrc_add_frame_flags(_buffersrc_ctx, f, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
        WarnL << "TranscodeOverlay: buffersrc add frame failed";
        return frame;
    }

    auto out = std::make_shared<FFmpegFrame>();
    int ret = av_buffersink_get_frame(_buffersink_ctx, out->get());
    if (ret < 0) {
        return frame;
    }
    // The filter creates a new AVFrame. Preserve all source properties, not
    // only pts/dts, before handing the frame to the encoder.
    ret = av_frame_copy_props(out->get(), f);
    if (ret < 0) {
        WarnL << "TranscodeOverlay: copy frame properties failed, ret=" << ret;
        return frame;
    }
    return out;
}

} // namespace mediakit

#endif // ENABLE_FFMPEG
