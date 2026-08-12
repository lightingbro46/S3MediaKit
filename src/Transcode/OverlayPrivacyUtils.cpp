#include "OverlayPrivacyUtils.h"

#include "Common/config.h"
#include "Util/File.h"
#include "Util/NoticeCenter.h"
#include "Util/base64.h"
#include "Util/logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>

using namespace std;
using namespace toolkit;

namespace OverlayPrivacyConfig {
#define OVERLAY_FIELD "overlay."

const string kEnableWatermark = OVERLAY_FIELD "enable_watermark";
const string kEnablePrivacyMask = OVERLAY_FIELD "enable_privacy_mask";
const string kUseWatermarkAsset = OVERLAY_FIELD "use_watermark_asset";
const string kOverlayRoot = OVERLAY_FIELD "overlay_root";

static onceToken token([]() {
    mINI::Instance()[kEnableWatermark] = true;
    mINI::Instance()[kEnablePrivacyMask] = true;
    mINI::Instance()[kUseWatermarkAsset] = true;
    mINI::Instance()[kOverlayRoot] = "./www/overlay/";
});
} // namespace OverlayPrivacyConfig

namespace mediakit {

namespace {

struct ComponentsPrepareState {
    std::vector<OverlayComponent> components;
    size_t next_image = 0;
    OverlayPrivacyUtils::ComponentsPrepareInvoker invoker;
    std::string overlay_root;
};

std::string safeAssetName(const std::string &asset_id) {
    std::string name;
    name.reserve(asset_id.size());
    for (size_t i = 0; i < asset_id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(asset_id[i]);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            name.push_back(static_cast<char>(c));
        } else {
            name.push_back('_');
        }
    }
    return name.empty() ? "overlay_image" : name;
}

std::string number(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << value;
    return out.str();
}

std::string escapeXml(const std::string &value) {
    std::string result;
    result.reserve(value.size() + 16);
    for (size_t i = 0; i < value.size(); ++i) {
        switch (value[i]) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += value[i]; break;
        }
    }
    return result;
}

std::string resolveTokens(const std::string &source,
                          const std::string &username,
                          const std::string &camera_name) {
    std::string result;
    for (size_t i = 0; i < source.size();) {
        if (source.compare(i, 13, "{{USER_NAME}}") == 0) {
            result += username;
            i += 13;
        } else if (source.compare(i, 15, "{{CAMERA_NAME}}") == 0) {
            result += camera_name;
            i += 15;
        } else {
            result += source[i++];
        }
    }
    return result;
}

std::string svgAttribute(const std::string &tag, const std::string &name) {
    const std::string key = name + "=";
    size_t pos = tag.find(key);
    if (pos == std::string::npos) return std::string();
    pos += key.size();
    while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
    if (pos >= tag.size()) return std::string();
    const char quote = tag[pos] == '\'' || tag[pos] == '"' ? tag[pos++] : 0;
    const size_t end = quote ? tag.find(quote, pos) : tag.find_first_of(" \t\r\n>", pos);
    return tag.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

std::string svgStyle(const std::string &style, const std::string &name) {
    const std::string key = name + ":";
    size_t pos = style.find(key);
    if (pos == std::string::npos) return std::string();
    pos += key.size();
    const size_t end = style.find(';', pos);
    std::string value = style.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    const size_t first = value.find_first_not_of(" \t\r\n");
    const size_t last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
}

// FFmpeg's librsvg decoder does not render HTML foreignObject nodes. Camera
// watermark SVGs commonly use foreignObject/div for text, so convert those
// nodes to native SVG text before passing the asset to the movie filter.
std::string normalizeSvgText(const std::string &source) {
    std::string result;
    size_t cursor = 0;
    while (cursor < source.size()) {
        const size_t begin = source.find("<foreignObject", cursor);
        if (begin == std::string::npos) {
            result += source.substr(cursor);
            break;
        }
        result += source.substr(cursor, begin - cursor);
        const size_t open_end = source.find('>', begin);
        const size_t close = open_end == std::string::npos ? std::string::npos : source.find("</foreignObject>", open_end + 1);
        if (open_end == std::string::npos || close == std::string::npos) {
            result += source.substr(begin);
            break;
        }

        const std::string object_tag = source.substr(begin, open_end - begin + 1);
        const size_t div_begin = source.find("<div", open_end + 1);
        const size_t div_open_end = div_begin == std::string::npos ? std::string::npos : source.find('>', div_begin);
        const size_t div_close = div_open_end == std::string::npos ? std::string::npos : source.find("</div>", div_open_end + 1);
        if (div_begin == std::string::npos || div_open_end == std::string::npos || div_close == std::string::npos || div_close > close) {
            result += source.substr(begin, close + 16 - begin);
            cursor = close + 16;
            continue;
        }

        const std::string style = svgAttribute(source.substr(div_begin, div_open_end - div_begin + 1), "style");
        const std::string x = svgAttribute(object_tag, "x");
        const std::string y = svgAttribute(object_tag, "y");
        const std::string width = svgAttribute(object_tag, "width");
        const std::string height = svgAttribute(object_tag, "height");
        const std::string text = source.substr(div_open_end + 1, div_close - div_open_end - 1);
        const std::string font_family = svgStyle(style, "font-family");
        const std::string font_size = svgStyle(style, "font-size");
        const std::string font_weight = svgStyle(style, "font-weight");
        const std::string color = svgStyle(style, "color");

        // foreignObject/div is vertically centered by the browser's CSS box
        // model. SVG text uses a baseline, so place that baseline below the
        // box center by roughly 0.35 em to preserve the original position.
        const double font_size_value = std::atof(font_size.c_str());
        // The original div is left-aligned inside the foreignObject. Using
        // the box center here shifts the rendered text right by width / 2.
        const double text_x = std::atof(x.c_str());
        const double text_y = std::atof(y.c_str()) + std::atof(height.c_str()) / 2.0 + font_size_value * 0.35;
        result += "<text x=\"" + number(text_x) +
                  "\" y=\"" + number(text_y) +
                  "\" text-anchor=\"start\" fill=\"" +
                  escapeXml(color.empty() ? "#ffffff" : color) + "\"";
        if (!font_family.empty()) result += " font-family=\"" + escapeXml(font_family) + "\"";
        if (!font_size.empty()) result += " font-size=\"" + escapeXml(font_size) + "\"";
        if (!font_weight.empty()) result += " font-weight=\"" + escapeXml(font_weight) + "\"";
        result += ">" + text + "</text>";
        cursor = close + 16;
    }
    return result;
}

std::string definitionId(const OverlayComponent &component, size_t index) {
    std::ostringstream index_stream;
    index_stream << index;
    std::string id = component.id.empty() ? "component_" + index_stream.str() : component.id;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            id[i] = '_';
        }
    }
    return "wm_def_" + id;
}

std::string findLocalImagePath(const std::string &overlay_root, const std::string &image_id) {
    static const char *extensions[] = {"png", "jpg", "jpeg", "gif", "webp"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        const std::string path = overlay_root + safeAssetName(image_id) + "." + extensions[i];
        if (File::fileExist(path) && File::fileSize(path) > 0) {
            return path;
        }
    }
    return std::string();
}

std::string findLocalSvgPath(const std::string &overlay_root, const std::string &asset_id) {
    const std::string path = overlay_root + safeAssetName(asset_id) + ".svg";
    return File::fileExist(path) && File::fileSize(path) > 0 ? path : std::string();
}

#if defined(ENABLE_FFMPEG)
std::string shellQuote(const std::string &value) {
    std::string quoted("'");
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\'') {
            quoted += "'\\''";
        } else {
            quoted += value[i];
        }
    }
    quoted += "'";
    return quoted;
}

bool resizeImageToPng(const std::string &source_path, const std::string &target_path,
                      int target_width, int target_height) {
    const int width = std::max(1, target_width);
    const int height = std::max(1, target_height);
    const std::string command = "ffmpeg -hide_banner -loglevel error -y -i " + shellQuote(source_path) +
        " -vf \"scale=" + std::to_string(width) + ":" + std::to_string(height) +
        ":force_original_aspect_ratio=decrease,pad=" + std::to_string(width) + ":" +
        std::to_string(height) + ":(ow-iw)/2:(oh-ih)/2:color=0x00000000\" -frames:v 1 " +
        shellQuote(target_path);
    return std::system(command.c_str()) == 0 && File::fileExist(target_path) &&
           File::fileSize(target_path) > 0;
}
#endif

std::string resolveImagePath(const OverlayComponent &component) {
    if (!component.image_path.empty()) {
        return component.image_path.front() == '/' ? component.image_path :
               File::absolutePath(component.image_path, "");
    }
    if (component.image_id.empty()) {
        return std::string();
    }
    GET_CONFIG(std::string, overlay_root, OverlayPrivacyConfig::kOverlayRoot);
    const std::string path = findLocalImagePath(overlay_root, component.image_id);
    if (path.empty()) {
        return std::string();
    }
    return path.front() == '/' ? path : File::absolutePath(path, "");
}

std::string mimeTypeForPath(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string() : path.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); ++i) {
        ext[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
    }
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "svg") return "image/svg+xml";
    return "image/png";
}

// librsvg renders the SVG via rsvg_handle_new_from_data() without a base URI,
// so it refuses to load external file references (including absolute paths)
// as an SSRF/LFI sandboxing measure. Embedding the image bytes as a data URI
// avoids external resource loading entirely, which is why raster images were
// silently missing from the rendered overlay while text still worked.
std::string imageHref(const OverlayComponent &component) {
    const std::string path = resolveImagePath(component);
    if (path.empty()) {
        return std::string();
    }
    const std::string content = File::loadFile(path);
    if (content.empty()) {
        WarnL << "OverlayPrivacyUtils: cannot read overlay image: " << path;
        return std::string();
    }
    return "data:" + mimeTypeForPath(path) + ";base64," + encodeBase64(content);
}

void prepareNextImage(const std::shared_ptr<ComponentsPrepareState> &state) {
    while (state->next_image < state->components.size()) {
        OverlayComponent &component = state->components[state->next_image];
        if (component.type != OverlayComponent::IMAGE || component.image_id.empty() ||
            !component.image_path.empty()) {
            ++state->next_image;
            continue;
        }

        const std::string local_path = state->overlay_root + safeAssetName(component.image_id) + ".download";
        const std::string cached_path = findLocalImagePath(state->overlay_root, component.image_id);
        component.image_path = cached_path.empty() ? local_path : cached_path;
        if (!cached_path.empty()) {
            ++state->next_image;
            continue;
        }

        const size_t image_index = state->next_image;
        Broadcast::DownloadFileInvoker download_invoker = [state, image_index, local_path](const std::string &err, const std::string &path) {
            if (!err.empty()) {
                state->invoker(err, std::vector<OverlayComponent>());
                return;
            }
            const std::string downloaded_path = path.empty() ? local_path : path;
            if (!File::fileExist(downloaded_path) || File::fileSize(downloaded_path) == 0) {
                state->invoker("Downloaded overlay image is empty: " + downloaded_path, std::vector<OverlayComponent>());
                return;
            }
            state->components[image_index].image_path = downloaded_path;
            ++state->next_image;
            prepareNextImage(state);
        };

        auto flag = NOTICE_EMIT(BroadcastDownloadOverlayImageArgs, Broadcast::kBroadcastDownloadOverlayImage, component.image_id, local_path, download_invoker);
        if (!flag) {
            state->invoker("No listener for kBroadcastDownloadOverlayImage", std::vector<OverlayComponent>());
        }
        return;
    }

    state->invoker("", state->components);
}

} // namespace

bool OverlayPrivacyUtils::parseComponents(const std::string &source,
                                          std::vector<OverlayComponent> &components,
                                          OverlayBuildOptions &options) {
    components.clear();
    if (source.empty()) {
        return false;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(source, root) || !root.isObject()) {
        return false;
    }
    options.canvas_width = root.get("canvas", Json::Value()).get("width", options.canvas_width).asInt();
    options.canvas_height = root.get("canvas", Json::Value()).get("height", options.canvas_height).asInt();
    const Json::Value &items = root["components"];
    if (!items.isArray()) {
        return false;
    }
    for (Json::ArrayIndex i = 0; i < items.size(); ++i) {
        const Json::Value &item = items[i];
        OverlayComponent component;
        std::string component_type = item.get("type", "TEXT").asString();
        for (size_t k = 0; k < component_type.size(); ++k) {
            component_type[k] = static_cast<char>(std::toupper(static_cast<unsigned char>(component_type[k])));
        }
        component.type = component_type == "IMAGE" ? OverlayComponent::IMAGE : OverlayComponent::TEXT;
        component.id = item.get("id", "component").asString();
        component.text = item.get("text", "").asString();
        component.font_family = item.get("fontFamily", "Arial").asString();
        component.font_size = item.get("fontSize", 34).asInt();
        component.font_weight = item.get("fontWeight", 700).asInt();
        component.color = item.get("color", "#ffffff").asString();
        component.image_id = item.get("imageId", "").asString();
        component.image_path = item.get("imagePath", "").asString();
        component.width = item.get("width", 120).asInt();
        component.height = item.get("height", 120).asInt();
        component.x = item.get("x", 0).asDouble();
        component.y = item.get("y", 0).asDouble();
        component.scale = item.get("scale", 1.0).asDouble();
        component.rotation = item.get("rotation", 0.0).asDouble();
        component.opacity = item.get("opacity", 1.0).asDouble();
        component.repeated = item.get("repeatEnabled", false).asBool();
        component.gap_x = item.get("gapX", 320).asInt();
        component.gap_y = item.get("gapY", 190).asInt();
        components.push_back(component);
    }
    return !components.empty();
}

bool OverlayPrivacyUtils::parseWatermarkAssetId(const std::string &source,
                                                std::string &asset_id) {
    asset_id.clear();
    if (source.empty()) {
        return false;
    }
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(source, root) || !root.isObject()) {
        return false;
    }
    asset_id = root.get("overlayAssetId", "").asString();
    return !asset_id.empty();
}

void OverlayPrivacyUtils::prepareWatermarkAsset(const std::string &source,
                                                const WatermarkAssetPrepareInvoker &invoker) {
    std::string asset_id;
    if (!parseWatermarkAssetId(source, asset_id)) {
        invoker("Watermark config does not contain overlayAssetId", "");
        return;
    }

    GET_CONFIG(std::string, overlay_root, OverlayPrivacyConfig::kOverlayRoot);
    if (overlay_root.empty() || !File::create_path(overlay_root, 0755)) {
        invoker("Cannot create overlay root: " + overlay_root, "");
        return;
    }

    const std::string cached_path = findLocalSvgPath(overlay_root, asset_id);
    if (!cached_path.empty()) {
        invoker("", cached_path);
        return;
    }

    const std::string local_path = overlay_root + safeAssetName(asset_id) + ".svg.download";
    Broadcast::DownloadFileInvoker download_invoker = [invoker, local_path, asset_id](const std::string &err, const std::string &path) {
        if (!err.empty()) {
            invoker(err, "");
            return;
        }
        const std::string downloaded_path = path.empty() ? local_path : path;
        if (!File::fileExist(downloaded_path) || File::fileSize(downloaded_path) == 0) {
            invoker("Downloaded watermark SVG is empty: " + downloaded_path, "");
            return;
        }
        const std::string svg_path = downloaded_path.size() >= 4 &&
            downloaded_path.compare(downloaded_path.size() - 4, 4, ".svg") == 0
            ? downloaded_path : local_path.substr(0, local_path.size() - 9);
        if (svg_path != downloaded_path) {
            if (File::fileExist(svg_path)) {
                File::delete_file(svg_path, false, false);
            }
            if (std::rename(downloaded_path.c_str(), svg_path.c_str()) != 0) {
                invoker("Cannot save watermark SVG asset: " + asset_id, "");
                return;
            }
        }
        invoker("", svg_path);
    };

    auto flag = NOTICE_EMIT(BroadcastDownloadOverlayImageArgs, Broadcast::kBroadcastDownloadOverlayImage,
                            asset_id, local_path, download_invoker);
    if (!flag) {
        invoker("No listener for kBroadcastDownloadOverlayImage", "");
    }
}

bool OverlayPrivacyUtils::resolveWatermarkAsset(const std::string &source,
                                                OverlayBuildOptions &options,
                                                std::string &resolved_path,
                                                std::string &error) {
    resolved_path.clear();
    error.clear();
    std::string asset_id;
    if (!parseWatermarkAssetId(source, asset_id)) {
        error = "Watermark config does not contain overlayAssetId";
        return false;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(source, root) || !root.isObject()) {
        error = "Watermark template is invalid";
        return false;
    }
    options.canvas_width = root.get("canvas", Json::Value()).get("width", options.canvas_width).asInt();
    options.canvas_height = root.get("canvas", Json::Value()).get("height", options.canvas_height).asInt();

    GET_CONFIG(std::string, overlay_root, OverlayPrivacyConfig::kOverlayRoot);
    const std::string raw_path = findLocalSvgPath(overlay_root, asset_id);
    if (raw_path.empty()) {
        error = "Watermark SVG asset is not prefetched: " + asset_id;
        return false;
    }
    const std::string raw_svg = File::loadFile(raw_path);
    if (raw_svg.empty()) {
        error = "Cannot read watermark SVG asset: " + raw_path;
        return false;
    }

    const std::string normalized_svg = normalizeSvgText(raw_svg);
    const std::string resolved_svg = options.resolve_dynamic_tokens
        ? resolveTokens(normalized_svg, options.username, options.camera_name) : normalized_svg;
    if (resolved_svg == raw_svg) {
        resolved_path = raw_path;
        return true;
    }

    const std::string suffix = safeAssetName(options.camera_name + "_" + options.username);
    resolved_path = overlay_root + safeAssetName(asset_id) + ".resolved." + suffix + ".svg";
    if (!File::saveFile(resolved_svg, resolved_path)) {
        error = "Cannot save resolved watermark SVG: " + resolved_path;
        resolved_path.clear();
        return false;
    }
    return true;
}

bool OverlayPrivacyUtils::parsePrivacyMasks(const std::string &source,
                                            std::vector<PrivacyMaskRegion> &masks,
                                            const OverlayBuildOptions &options) {
    masks.clear();
    if (source.empty()) return false;
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(source, root) || !root.isArray()) return false;
    const double width = std::max(1, options.canvas_width);
    const double height = std::max(1, options.canvas_height);
    for (Json::ArrayIndex i = 0; i < root.size(); ++i) {
        const Json::Value &item = root[i];
        const Json::Value &points = item["points"];
        if (!points.isArray() || points.size() < 3) continue;
        PrivacyMaskRegion mask;
        mask.id = item.get("id", "privacy_mask").asString();
        mask.color = item.get("color", "#000000").asString();
        mask.opacity = item.get("opacity", 1.0).asDouble();
        std::string mask_type = item.get("maskType", "SOLID").asString();
        for (size_t k = 0; k < mask_type.size(); ++k) {
            mask_type[k] = static_cast<char>(std::toupper(static_cast<unsigned char>(mask_type[k])));
        }
        if (mask_type == "BLUR") {
            mask.mask_type = PrivacyMaskRegion::BLUR;
        } else if (mask_type == "PIXELATE") {
            mask.mask_type = PrivacyMaskRegion::PIXELATE;
        }
        for (Json::ArrayIndex j = 0; j < points.size(); ++j) {
            double x = points[j].get("x", 0.0).asDouble();
            double y = points[j].get("y", 0.0).asDouble();
            if (x >= 0.0 && x <= 1.0) x *= width;
            if (y >= 0.0 && y <= 1.0) y *= height;
            mask.points.push_back(std::make_pair(x, y));
        }
        if (mask.points.size() >= 3) masks.push_back(mask);
    }
    TraceL << "Parsed privacy masks: total=" << masks.size();
    for (size_t i = 0; i < masks.size(); ++i) {
        const char *type = masks[i].mask_type == PrivacyMaskRegion::BLUR ? "BLUR" :
                           masks[i].mask_type == PrivacyMaskRegion::PIXELATE ? "PIXELATE" : "SOLID";
        TraceL << "Privacy mask id=" << masks[i].id << ", type=" << type << ", points=" << masks[i].points.size();
    }
    return !masks.empty();
}

std::string OverlayPrivacyUtils::buildSvg(const std::vector<OverlayComponent> &components,
                                           const OverlayBuildOptions &options) {
    const int canvas_width = std::max(1, options.canvas_width);
    const int canvas_height = std::max(1, options.canvas_height);
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        << "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        << "width=\"" << canvas_width << "\" height=\"" << canvas_height
        << "\" viewBox=\"0 0 " << canvas_width << " " << canvas_height << "\">\n";

    if (options.include_background) {
        svg << "<rect width=\"" << canvas_width << "\" height=\"" << canvas_height
            << "\" fill=\"" << escapeXml(options.background_color) << "\"/>\n"
            << "<rect width=\"" << canvas_width << "\" height=\"" << canvas_height
            << "\" fill=\"none\" stroke=\"" << escapeXml(options.border_color) << "\"/>\n";
    }

    // Privacy masks are burned in separately (pixel processing for live transcode,
    // ffmpeg filter_complex for extract) — this SVG only carries the watermark.
    svg << "<defs>\n";
    for (size_t i = 0; i < components.size(); ++i) {
        const OverlayComponent &component = components[i];
        const std::string id = definitionId(component, i);
        svg << "<g id=\"" << escapeXml(id) << "\">\n";
        if (component.type == OverlayComponent::TEXT) {
            std::string text = component.text;
            if (options.resolve_dynamic_tokens) {
                text = resolveTokens(text, options.username, options.camera_name);
            }
            svg << "<text x=\"0\" y=\"0\" fill=\"" << escapeXml(component.color)
                << "\" font-family=\"" << escapeXml(component.font_family)
                << "\" font-size=\"" << std::max(1, component.font_size)
                << "\" font-weight=\"" << component.font_weight << "\">"
                << escapeXml(text) << "</text>\n";
        } else {
            const std::string href = imageHref(component);
            svg << "<image href=\"" << escapeXml(href) << "\" xlink:href=\""
                << escapeXml(href) << "\" x=\"0\" y=\"0\" width=\""
                << std::max(1, component.width) << "\" height=\""
                << std::max(1, component.height)
                << "\" preserveAspectRatio=\"xMidYMid meet\"/>\n";
        }
        svg << "</g>\n";
    }
    svg << "</defs>\n";

    for (size_t i = 0; i < components.size(); ++i) {
        const OverlayComponent &component = components[i];
        const int gap_x = std::max(1, component.gap_x);
        const int gap_y = std::max(1, component.gap_y);
        const int columns = component.repeated ? canvas_width / gap_x + 3 : 0;
        const int rows = component.repeated ? canvas_height / gap_y + 3 : 0;
        const double opacity = std::max(0.0, std::min(1.0, component.opacity));

        for (int row = component.repeated ? -rows : 0; row <= (component.repeated ? rows : 0); ++row) {
            for (int column = component.repeated ? -columns : 0;
                 column <= (component.repeated ? columns : 0); ++column) {
                const double offset_x = column * gap_x;
                const double offset_y = row * gap_y;
                const double center_x = component.type == OverlayComponent::IMAGE
                    ? std::max(1, component.width) / 2.0
                    : std::max(24, static_cast<int>(component.text.size() * std::max(1, component.font_size) * 0.6 + 8)) / 2.0;
                const double center_y = component.type == OverlayComponent::IMAGE
                    ? std::max(1, component.height) / 2.0
                    : std::max(24, component.font_size + 8) / 2.0;
                const std::string transform = "translate(" + number(component.x + offset_x) +
                    " " + number(component.y + offset_y) + ") translate(" + number(center_x) +
                    " " + number(center_y) + ") rotate(" + number(component.rotation) +
                    ") scale(" + number(component.scale) + ") translate(" + number(-center_x) +
                    " " + number(-center_y) + ")";
                if (component.type == OverlayComponent::IMAGE) {
                    // Do not reference an image through <use>. FFmpeg's SVG
                    // renderer handles direct image nodes reliably, while an
                    // image nested in <defs>/<use> may be silently skipped.
                    const std::string href = imageHref(component);
                    svg << "<image href=\"" << escapeXml(href)
                        << "\" xlink:href=\"" << escapeXml(href)
                        << "\" x=\"0\" y=\"0\" width=\"" << std::max(1, component.width)
                        << "\" height=\"" << std::max(1, component.height)
                        << "\" preserveAspectRatio=\"xMidYMid meet\" transform=\""
                        << transform << "\" opacity=\"" << number(opacity) << "\"/>\n";
                } else {
                    svg << "<use href=\"#" << escapeXml(definitionId(component, i))
                        << "\" xlink:href=\"#" << escapeXml(definitionId(component, i))
                        << "\" transform=\"" << transform << "\" opacity=\""
                        << number(opacity) << "\"/>\n";
                }
            }
        }
    }
    svg << "</svg>";
    return svg.str();
}

std::string OverlayPrivacyUtils::buildPrivacyMaskSvg(const std::vector<PrivacyMaskRegion> &masks,
                                                      int canvas_width, int canvas_height,
                                                      PrivacyMaskRegion::MaskType mask_type,
                                                      bool alpha_only) {
    const int width = std::max(1, canvas_width);
    const int height = std::max(1, canvas_height);
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width
        << "\" height=\"" << height << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";

    for (size_t i = 0; i < masks.size(); ++i) {
        const PrivacyMaskRegion &mask = masks[i];
        if (mask.mask_type != mask_type || mask.points.size() < 3) {
            continue;
        }
        svg << "<polygon points=\"";
        for (size_t p = 0; p < mask.points.size(); ++p) {
            if (p) svg << " ";
            svg << number(mask.points[p].first) << "," << number(mask.points[p].second);
        }
        svg << "\" fill=\"" << (alpha_only ? "#ffffff" : escapeXml(mask.color))
            << "\" fill-opacity=\"" << number(std::max(0.0, std::min(1.0, mask.opacity)))
            << "\" fill-rule=\"nonzero\"/>\n";
    }
    svg << "</svg>\n";
    return svg.str();
}

void OverlayPrivacyUtils::prepareComponents(const std::string &source,
                                            std::vector<OverlayComponent> &components,
                                            OverlayBuildOptions &options,
                                            const ComponentsPrepareInvoker &invoker) {
    components.clear();
    if (!parseComponents(source, components, options)) {
        invoker("Watermark template is invalid", std::vector<OverlayComponent>());
        return;
    }

    GET_CONFIG(std::string, overlay_root, OverlayPrivacyConfig::kOverlayRoot);
    if (overlay_root.empty() || !File::create_path(overlay_root, 0755)) {
        invoker("Cannot create overlay root: " + overlay_root, std::vector<OverlayComponent>());
        return;
    }

    auto state = std::make_shared<ComponentsPrepareState>();
    state->components = components;
    state->invoker = invoker;
    state->overlay_root = overlay_root;
    prepareNextImage(state);
}

bool OverlayPrivacyUtils::resolveLocalImages(std::vector<OverlayComponent> &components,
                                             std::string &error) {
    GET_CONFIG(std::string, overlay_root, OverlayPrivacyConfig::kOverlayRoot);
    if (overlay_root.empty() || !File::is_dir(overlay_root)) {
        error = "Overlay root is unavailable: " + overlay_root;
        return false;
    }

    for (size_t i = 0; i < components.size(); ++i) {
        OverlayComponent &component = components[i];
        if (component.type != OverlayComponent::IMAGE || component.image_id.empty()) {
            continue;
        }
        if (component.image_path.empty()) {
            component.image_path = findLocalImagePath(overlay_root, component.image_id);
            if (component.image_path.empty()) {
                component.image_path = overlay_root + safeAssetName(component.image_id) + ".download";
            }
        }
        if (component.image_path.front() != '/') {
            const std::string absolute_path = File::absolutePath(component.image_path, "");
            if (File::fileExist(absolute_path) && File::fileSize(absolute_path) > 0) {
                component.image_path = absolute_path;
            }
        }
        if (!File::fileExist(component.image_path) || File::fileSize(component.image_path) == 0) {
            error = "Overlay image is not prefetched: " + component.image_id;
            return false;
        }
#if defined(ENABLE_FFMPEG)
        const int cache_width = std::max(1, component.width);
        const int cache_height = std::max(1, component.height);
        const std::string resized_path = overlay_root + safeAssetName(component.image_id) + "." +
                                          std::to_string(cache_width) + "x" +
                                          std::to_string(cache_height) + ".png";
        if (!File::fileExist(resized_path) || File::fileSize(resized_path) == 0) {
            if (!resizeImageToPng(component.image_path, resized_path, cache_width, cache_height)) {
                WarnL << "Cannot create resized overlay image cache: " << component.image_path
                      << " -> " << resized_path;
            }
        }
        if (File::fileExist(resized_path) && File::fileSize(resized_path) > 0) {
            component.image_path = resized_path;
        }
#endif
    }
    return true;
}

std::string OverlayPrivacyUtils::escapeMoviePath(const std::string &path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '\\' || c == ':' || c == '\'') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

namespace {

std::string fracStr(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(6) << v;
    return o.str();
}

// ffmpeg drawbox color syntax: 0xRRGGBB[@alpha].
std::string colorToFFmpegColor(const std::string &hex, double opacity) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') {
        h = h.substr(1);
    }
    if (h.size() != 6) {
        h = "000000";
    }
    return "0x" + h + "@" + fracStr(std::max(0.0, std::min(1.0, opacity)));
}

// Axis-aligned bounding box of the mask polygon, as [0,1] fractions of the reference canvas.
// Used because ffmpeg's crop/drawbox filters only operate on rectangles, not arbitrary polygons.
bool maskBBoxFractions(const PrivacyMaskRegion &mask, int canvas_w, int canvas_h,
                       double &xf, double &yf, double &wf, double &hf) {
    if (mask.points.size() < 3) {
        return false;
    }
    double min_x = mask.points[0].first, max_x = min_x;
    double min_y = mask.points[0].second, max_y = min_y;
    for (const auto &p : mask.points) {
        min_x = std::min(min_x, p.first);
        max_x = std::max(max_x, p.first);
        min_y = std::min(min_y, p.second);
        max_y = std::max(max_y, p.second);
    }
    min_x = std::max(0.0, std::min((double)canvas_w, min_x));
    max_x = std::max(0.0, std::min((double)canvas_w, max_x));
    min_y = std::max(0.0, std::min((double)canvas_h, min_y));
    max_y = std::max(0.0, std::min((double)canvas_h, max_y));
    if (max_x <= min_x || max_y <= min_y) {
        return false;
    }
    xf = min_x / canvas_w;
    yf = min_y / canvas_h;
    wf = (max_x - min_x) / canvas_w;
    hf = (max_y - min_y) / canvas_h;
    return true;
}

} // namespace

std::string OverlayPrivacyUtils::buildPrivacyMaskFilterComplex(const std::vector<PrivacyMaskRegion> &masks,
                                                                int canvas_width, int canvas_height,
                                                                std::string &last_label) {
    std::vector<std::string> clauses;
    std::string cur = "0:v";
    int idx = 0;
    for (const auto &mask : masks) {
        double xf, yf, wf, hf;
        if (!maskBBoxFractions(mask, canvas_width, canvas_height, xf, yf, wf, hf)) {
            continue;
        }
        std::string next = "pm" + std::to_string(idx++);
        if (mask.mask_type == PrivacyMaskRegion::SOLID) {
            std::ostringstream expr;
            expr << "[" << cur << "]drawbox=x='iw*" << fracStr(xf) << "':y='ih*" << fracStr(yf)
                 << "':w='iw*" << fracStr(wf) << "':h='ih*" << fracStr(hf)
                 << "':color=" << colorToFFmpegColor(mask.color, mask.opacity) << ":t=fill[" << next << "]";
            clauses.push_back(expr.str());
        } else {
            // Split the running stream so the region can be cropped/processed independently, then
            // overlaid back onto the untouched copy.
            clauses.push_back("[" + cur + "]split=2[" + next + "_base][" + next + "_src]");

            std::ostringstream proc;
            proc << "[" << next << "_src]crop=w='iw*" << fracStr(wf) << "':h='ih*" << fracStr(hf)
                 << "':x='iw*" << fracStr(xf) << "':y='ih*" << fracStr(yf) << "'";
            if (mask.mask_type == PrivacyMaskRegion::BLUR) {
                // TranscodeOverlay applies two radius-2 box passes. Use the equivalent
                // FFmpeg settings instead of the stronger radius-12/power-2 blur.
                proc << ",boxblur=luma_radius=2:luma_power=1:chroma_radius=2:chroma_power=1";
            } else {
                // Pixelate = area-average downscale to 12-pixel blocks, then nearest-neighbor upscale.
                // This follows TranscodeOverlay's 12x12 luma blocks more closely than factor 16.
                // The comma inside
                // max(...) must reach ffmpeg's own filter-graph parser as a literal (not a filter-chain
                // separator); our own command-line tokenizer (Process::run -> parse_shell_like) strips
                // bare quote characters, so escape them with a backslash to survive as literal '...' quotes.
                proc << ",scale=w=\\'max(1,trunc(iw/12))\\':h=\\'max(1,trunc(ih/12))\\':flags=area"
                        ",scale=w=iw*12:h=ih*12:flags=neighbor";
            }
            proc << "[" << next << "_proc]";
            clauses.push_back(proc.str());

            std::ostringstream overlay;
            overlay << "[" << next << "_base][" << next << "_proc]overlay=x='main_w*" << fracStr(xf)
                    << "':y='main_h*" << fracStr(yf) << "'[" << next << "]";
            clauses.push_back(overlay.str());
        }
        cur = next;
    }
    last_label = cur;
    if (clauses.empty()) {
        return "";
    }
    std::ostringstream out;
    for (size_t i = 0; i < clauses.size(); ++i) {
        if (i) out << ";";
        out << clauses[i];
    }
    return out.str();
}

} // namespace mediakit
