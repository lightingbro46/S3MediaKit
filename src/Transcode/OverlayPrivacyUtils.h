#ifndef TRANSCODE_OVERLAYPRIVACYUTILS_H
#define TRANSCODE_OVERLAYPRIVACYUTILS_H

#include <functional>
#include <string>
#include <vector>

namespace OverlayPrivacyConfig {
// Whether to enable watermark overlay on the video stream.
extern const std::string kEnableWatermark;
// Whether to enable privacy mask overlay on the video stream.
extern const std::string kEnablePrivacyMask;
// Local directory used to cache downloaded watermark/image overlay assets.
extern const std::string kOverlayRoot;

} // namespace OverlayPrivacyConfig

namespace mediakit {

/** A text or image element used to build a transparent overlay SVG. */
struct OverlayComponent {
    enum Type {
        TEXT,
        IMAGE
    };

    Type type = TEXT;
    std::string id;

    // Text properties.
    std::string text;
    std::string font_family = "Arial";
    int font_size = 34;
    int font_weight = 700;
    std::string color = "#ffffff";

    // Image properties.
    std::string image_id;
    std::string image_path;
    int width = 120;
    int height = 120;

    // Position and transform properties.
    double x = 0.0;
    double y = 0.0;
    double scale = 1.0;
    double rotation = 0.0;
    double opacity = 1.0;

    // Repetition properties.
    bool repeated = false;
    int gap_x = 320;
    int gap_y = 190;
};

struct PrivacyMaskRegion {
    enum MaskType {
        SOLID,
        BLUR,
        PIXELATE
    };

    std::string id;
    std::vector<std::pair<double, double> > points;
    std::string color = "#000000";
    double opacity = 1.0;
    MaskType mask_type = SOLID;
};

struct OverlayBuildOptions {
    int canvas_width = 1280;
    int canvas_height = 720;
    bool include_background = false;
    bool resolve_dynamic_tokens = false;
    std::string background_color = "#252525";
    std::string border_color = "#5e5e5e";
    std::string username;
    std::string camera_name;
    std::vector<PrivacyMaskRegion> privacy_masks;
};

/**
 * C++11-only SVG builder for privacy masks and watermarks.
 *
 * The generated SVG is transparent by default and can be consumed by an
 * FFmpeg movie filter. This class intentionally has no Qt dependency.
 */
class OverlayPrivacyUtils {
public:
    using ComponentsPrepareInvoker = std::function<void(const std::string &, const std::vector<OverlayComponent> &)>;

    /**
     * Parse a watermark template and ensure all image components are available
     * locally before the SVG is generated. The download is delegated through
     * kBroadcastDownloadOverlayImage so this utility does not depend on the
     * manager or HTTP hook implementation.
     */
    static void prepareComponents(const std::string &source,
                                  std::vector<OverlayComponent> &components,
                                  OverlayBuildOptions &options,
                                  const ComponentsPrepareInvoker &invoker);

    /** Resolve image components to the prefetched local cache without downloading. */
    static bool resolveLocalImages(std::vector<OverlayComponent> &components,
                                   std::string &error);

    static bool parseComponents(const std::string &source,
                                std::vector<OverlayComponent> &components,
                                OverlayBuildOptions &options);

    static bool parsePrivacyMasks(const std::string &source,
                                  std::vector<PrivacyMaskRegion> &masks,
                                  const OverlayBuildOptions &options);

    static std::string buildSvg(const std::vector<OverlayComponent> &components,
                                const OverlayBuildOptions &options = OverlayBuildOptions());

    /** Build a transparent SVG containing the configured privacy polygons. */
    static std::string buildPrivacyMaskSvg(const std::vector<PrivacyMaskRegion> &masks,
                                           int canvas_width, int canvas_height,
                                           PrivacyMaskRegion::MaskType mask_type,
                                           bool alpha_only);

    /** Escape a path for use in an avfilter movie= option (backslash, colon, single-quote). */
    static std::string escapeMoviePath(const std::string &path);

    /**
     * Build a real ffmpeg filter_complex chain that burns each privacy mask region into the video:
     * drawbox for SOLID, crop+boxblur+overlay for BLUR, crop+downscale/upscale+overlay for PIXELATE.
     * Regions are approximated by their polygon's bounding box since ffmpeg's rect filters can't
     * clip to an arbitrary polygon. Writes the label of the final processed video stream to
     * last_label ("0:v" unchanged when there is nothing to draw). Returns an empty string when
     * masks is empty.
     */
    static std::string buildPrivacyMaskFilterComplex(const std::vector<PrivacyMaskRegion> &masks,
                                                     int canvas_width, int canvas_height,
                                                     std::string &last_label);

};

} // namespace mediakit

#endif // TRANSCODE_OVERLAYPRIVACYUTILS_H
