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
// Use the SVG identified by watermark config's overlayAssetId instead of
// generating an SVG from the components array.
extern const std::string kUseWatermarkAsset;
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
    // Resolved, locally cached SVG used when the watermark config contains
    // overlayAssetId and overlay.use_watermark_asset is enabled.
    std::string prebuilt_svg_path;
    std::vector<PrivacyMaskRegion> privacy_masks;
};

/**
 * C++11-only SVG builder for watermarks (buildSvg draws components only \u2014 privacy
 * masks are burned in separately via pixel processing or ffmpeg filters, see
 * TranscodeOverlay::applyPrivacyMasks).
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

    static bool parseWatermarkAssetId(const std::string &source,
                                      std::string &asset_id);

    /** Download/cache the raw SVG asset identified by overlayAssetId. */
    using WatermarkAssetPrepareInvoker = std::function<void(const std::string &, const std::string &)>;
    static void prepareWatermarkAsset(const std::string &source,
                                      const WatermarkAssetPrepareInvoker &invoker);

    /** Resolve the cached SVG and substitute dynamic tokens for this camera. */
    static bool resolveWatermarkAsset(const std::string &source,
                                      OverlayBuildOptions &options,
                                      std::string &resolved_path,
                                      std::string &error);

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

};

} // namespace mediakit

#endif // TRANSCODE_OVERLAYPRIVACYUTILS_H
