#ifndef TRANSCODE_TRANSCODEOVERLAY_H
#define TRANSCODE_TRANSCODEOVERLAY_H

#if defined(ENABLE_FFMPEG)

#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "Codec/Transcode.h"
#include "OverlayPrivacyUtils.h"

namespace mediakit {

/**
 * Applies privacy masks directly to YUV frames and renders watermark
 * components through a small avfilter overlay graph.
 *
 * The watermark graph is built lazily on the first frame and whenever the
 * input resolution/format changes. Thread-affinity: call inputFrame() from a
 * single decode thread. If an asset cannot be loaded, processing degrades
 * gracefully to pass-through.
 */
class TranscodeOverlay {
public:
    using Ptr = std::shared_ptr<TranscodeOverlay>;

    /**
     * @param image_path  Path to an image with alpha channel (PNG recommended).
     * @param x           Overlay X offset in pixels.
     * @param y           Overlay Y offset in pixels.
     */
    TranscodeOverlay(std::string image_path, int x = 0, int y = 0);
    TranscodeOverlay(const std::vector<OverlayComponent> &components,
                     const OverlayBuildOptions &options = OverlayBuildOptions());
    ~TranscodeOverlay();

    /**
     * Overlay the configured image onto @p frame.
     * @return The overlaid frame, or the original frame on failure / when disabled.
     */
    FFmpegFrame::Ptr inputFrame(const FFmpegFrame::Ptr &frame);

    /** True when the overlay image was resolved and the feature is usable. */
    bool valid() const { return _valid; }

private:
    bool buildGraph(const FFmpegFrame::Ptr &frame);
    void freeGraph();

private:
    struct CachedPrivacyMask {
        struct PlaneCache {
            int min_x = 0;
            int min_y = 0;
            int max_x = -1;
            int max_y = -1;
            std::vector<std::vector<std::pair<int, int> > > spans;
            // Reused by BLUR and PIXELATE. Capacity is retained across frames.
            std::vector<uint8_t> work;
            std::vector<uint8_t> temp;
        };

        PrivacyMaskRegion config;
        int min_x = 0;
        int min_y = 0;
        int max_x = 0;
        int max_y = 0;
        uint8_t fill[3] = {0, 128, 128};
        uint8_t alpha = 255;
        PlaneCache planes[3];
    };

    std::string _image_path;
    std::string _generated_image_path;
    int _x = 0;
    int _y = 0;
    bool _valid = false;

    int _src_width = 0;
    int _src_height = 0;
    int _src_format = -1;
    size_t _overlay_component_count = 0;
    size_t _privacy_mask_count = 0;
    int _overlay_canvas_width = 1280;
    int _overlay_canvas_height = 720;
    std::vector<PrivacyMaskRegion> _privacy_masks;
    std::vector<CachedPrivacyMask> _cached_privacy_masks;
    FFmpegSws::Ptr _privacy_sws;
    int _privacy_width = 0;
    int _privacy_height = 0;

    AVFilterGraph *_graph = nullptr;
    AVFilterContext *_buffersrc_ctx = nullptr;
    AVFilterContext *_buffersink_ctx = nullptr;

    bool buildPrivacyMaskCache(int width, int height);
    FFmpegFrame::Ptr applyPrivacyMasks(const FFmpegFrame::Ptr &frame);
};

} // namespace mediakit

#endif // ENABLE_FFMPEG

#endif // TRANSCODE_TRANSCODEOVERLAY_H
