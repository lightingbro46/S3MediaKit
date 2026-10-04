#include "PsiaDeviceControl.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <strings.h>
#include <thread>
#include "stdsoap2.h"
#include "plugin/httpda.h"
#include "Onvif/pugixml.hpp"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace PsiaCtrl {
#define PSIA_CTRL_FIELD "psia."
const string kRequestTimeoutMs = PSIA_CTRL_FIELD"request_timeout_ms";

static onceToken token([]() {
    // A PSIA request is one small HTTP exchange; a camera that needs longer is treated as unreachable.
    mINI::Instance()[kRequestTimeoutMs] = 5000;
});
} // namespace PsiaCtrl

namespace IsapiCtrl {
#define ISAPI_CTRL_FIELD "isapi."
const string kRequestTimeoutMs = ISAPI_CTRL_FIELD"request_timeout_ms";

static onceToken token([]() {
    mINI::Instance()[kRequestTimeoutMs] = 5000;
});
} // namespace IsapiCtrl

namespace managerkit {

namespace psia_detail {

// ── XML helpers: match on local names, cameras disagree on namespaces and prefixes ──────────────

static const char *localName(const pugi::xml_node &node) {
    const char *name = node.name();
    const char *colon = strchr(name, ':');
    return colon ? colon + 1 : name;
}

static pugi::xml_node child(const pugi::xml_node &node, const char *name) {
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_element && strcmp(localName(c), name) == 0) {
            return c;
        }
    }
    return pugi::xml_node();
}

// Depth-first search for the first element called `name`, `node` included.
static pugi::xml_node find(const pugi::xml_node &node, const char *name) {
    if (node.type() == pugi::node_element && strcmp(localName(node), name) == 0) {
        return node;
    }
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
        pugi::xml_node found = find(c, name);
        if (found) {
            return found;
        }
    }
    return pugi::xml_node();
}

static void findAll(const pugi::xml_node &node, const char *name, std::vector<pugi::xml_node> &out) {
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
        if (c.type() != pugi::node_element) {
            continue;
        }
        if (strcmp(localName(c), name) == 0) {
            out.push_back(c);
        } else {
            findAll(c, name, out);
        }
    }
}

static std::string text(const pugi::xml_node &node, const char *name) {
    pugi::xml_node c = child(node, name);
    if (!c) {
        return "";
    }
    std::string value = c.text().get();
    value.erase(0, value.find_first_not_of(" \t\r\n"));
    value.erase(value.find_last_not_of(" \t\r\n") + 1);
    return value;
}

static bool load(pugi::xml_document &doc, const std::string &xml) {
    return !xml.empty() && doc.load_buffer(xml.data(), xml.size());
}

static pugi::xml_node root(const pugi::xml_document &doc) {
    return doc.document_element();
}

static bool isTrue(const std::string &v) {
    return v == "true" || v == "1" || v == "TRUE" || v == "True";
}

// Range of a "XRange"/"YRange"/"ZRange" child: <XRange><Min>..</Min><Max>..</Max></XRange>.
static bool range(const pugi::xml_node &space, const char *name, float &min, float &max) {
    pugi::xml_node r = child(space, name);
    if (!r) {
        return false;
    }
    std::string lo = text(r, "Min"), hi = text(r, "Max");
    if (lo.empty() || hi.empty()) {
        return false;
    }
    min = strtof(lo.c_str(), nullptr);
    max = strtof(hi.c_str(), nullptr);
    return true;
}

// ── parsing ───────────────────────────────────────────────────────────────────────────────────

bool parseResponseStatus(const std::string &xml, ResponseStatus &out) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return false;
    }
    pugi::xml_node node = find(root(doc), "ResponseStatus");
    if (!node) {
        return false;
    }
    out.statusCode = atoi(text(node, "statusCode").c_str());
    out.statusString = text(node, "statusString");
    out.subStatusCode = text(node, "subStatusCode");
    return true;
}

bool isResponseStatusOk(const ResponseStatus &status) {
    return status.statusCode == 1 || status.statusCode == 7;
}

bool parseDeviceInfo(const std::string &xml, OnvifDeviceInfo &out, const std::string &default_manufacturer) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return false;
    }
    pugi::xml_node node = find(root(doc), "DeviceInfo");
    if (!node) {
        return false;
    }
    out.manufacturer = text(node, "manufacturer");
    if (out.manufacturer.empty()) {
        out.manufacturer = default_manufacturer.empty() ? text(node, "deviceName") : default_manufacturer;
    }
    if (out.manufacturer.empty()) {
        out.manufacturer = "PSIA";
    }
    out.model = text(node, "model");
    out.firmwareVersion = text(node, "firmwareVersion");
    out.serialNumber = text(node, "serialNumber");
    out.macAddress = text(node, "macAddress");
    out.hardwareId = text(node, "deviceID");
    return true;
}

std::string codecFromPsia(const std::string &psia) {
    std::string up;
    for (char c : psia) {
        if (c != '.' && c != ' ' && c != '+') {
            up += static_cast<char>(toupper(static_cast<unsigned char>(c)));
        }
    }
    if (up == "H264" || up == "SMART264") return "H264";
    if (up == "H265" || up == "HEVC" || up == "SMART265") return "H265";
    if (up == "MJPEG" || up == "JPEG") return "JPEG";
    if (up == "MPEG4") return "MPEG4";
    return psia;
}

std::string codecToPsia(const std::string &vcodec) {
    if (vcodec == "H264") return "H.264";
    if (vcodec == "H265") return "H.265";
    if (vcodec == "JPEG") return "MJPEG";
    if (vcodec == "MPEG4") return "MPEG4";
    return vcodec;
}

static int bitrateOf(const pugi::xml_node &video) {
    std::string cbr = text(video, "constantBitRate");
    std::string vbr = text(video, "vbrUpperCap");
    bool use_vbr = text(video, "videoQualityControlType") == "VBR" || text(video, "videoQualityControlType") == "vbr";
    const std::string &first = use_vbr ? vbr : cbr;
    const std::string &second = use_vbr ? cbr : vbr;
    return atoi((!first.empty() ? first : second).c_str());
}

static bool parseChannel(const pugi::xml_node &channel, const std::string &host, const std::string &rtsp_path,
                         int default_rtsp_port, OnvifMediaProfile &profile) {
    pugi::xml_node video = child(channel, "Video");
    std::string id = text(channel, "id");
    if (!video || id.empty()) {
        return false;
    }
    std::string enabled = text(video, "enabled");
    if (!enabled.empty() && !isTrue(enabled)) {
        return false;
    }
    profile = OnvifMediaProfile();
    profile.token = id;
    profile.hasVideo = true;
    profile.vcodec = codecFromPsia(text(video, "videoCodecType"));
    profile.width = atoi(text(video, "videoResolutionWidth").c_str());
    profile.height = atoi(text(video, "videoResolutionHeight").c_str());
    profile.bitrate = bitrateOf(video);
    // maxFrameRate is in 1/100 fps.
    profile.fps = atoi(text(video, "maxFrameRate").c_str()) / 100.0f;
    profile.quality = 0;
    profile.channelNo = 0;

    pugi::xml_node audio = child(channel, "Audio");
    if (audio && isTrue(text(audio, "enabled"))) {
        profile.hasAudio = true;
        std::string acodec = text(audio, "audioCompressionType");
        profile.acodec = acodec.find("711") != std::string::npos ? "G711"
                       : acodec.find("726") != std::string::npos ? "G726"
                       : acodec.find("AAC") != std::string::npos ? "AAC" : acodec;
    }

    int rtsp_port = default_rtsp_port;
    pugi::xml_node transport = child(channel, "Transport");
    if (transport) {
        int port = atoi(text(transport, "rtspPortNo").c_str());
        if (port > 0) {
            rtsp_port = port;
        }
    }
    profile.url = "rtsp://" + host + ":" + std::to_string(rtsp_port) + rtsp_path + id;
    return true;
}

bool parseStreamingChannels(const std::string &xml, const std::string &host, OnvifMediaProfileMap &out,
                            const std::string &rtsp_path, int default_rtsp_port) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return false;
    }
    std::vector<pugi::xml_node> channels;
    pugi::xml_node top = root(doc);
    if (strcmp(localName(top), "StreamingChannel") == 0) {
        channels.push_back(top);
    } else if (strcmp(localName(top), "StreamingChannelList") == 0) {
        findAll(top, "StreamingChannel", channels);
    } else {
        return false;
    }
    out.clear();
    for (const auto &channel : channels) {
        OnvifMediaProfile profile;
        if (parseChannel(channel, host, rtsp_path, default_rtsp_port, profile)) {
            out.push_back(profile);
        }
    }
    return true;
}

static std::vector<std::string> splitOpt(const std::string &opt) {
    std::vector<std::string> out;
    std::stringstream ss(opt);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

void parseChannelCapabilities(const std::string &xml, OnvifMediaProfile &profile) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return;
    }
    pugi::xml_node video = find(root(doc), "Video");
    if (!video) {
        return;
    }
    VideoEncoderConfigOption option;
    std::vector<std::string> widths = splitOpt(child(video, "videoResolutionWidth").attribute("opt").value());
    std::vector<std::string> heights = splitOpt(child(video, "videoResolutionHeight").attribute("opt").value());
    // PSIA lists widths and heights separately; cameras list them pairwise, in the same order.
    for (size_t i = 0; i < widths.size() && i < heights.size(); ++i) {
        option.ResolutionsAvailable.emplace_back(atoi(widths[i].c_str()), atoi(heights[i].c_str()));
    }

    const char *rate_node = child(video, "constantBitRate") ? "constantBitRate" : "vbrUpperCap";
    pugi::xml_node rate = child(video, rate_node);
    int min_rate = rate.attribute("min").as_int(0);
    int max_rate = rate.attribute("max").as_int(0);
    option.BitRateRange = bitrateRangeToString(min_rate, max_rate);
    option.QualityRange = std::make_pair(0.0f, 0.0f);

    std::string fps_list;
    for (const auto &fps : splitOpt(child(video, "maxFrameRate").attribute("opt").value())) {
        int value = atoi(fps.c_str());
        if (value <= 0) {
            continue;
        }
        std::ostringstream one;
        one << value / 100.0f;
        fps_list += (fps_list.empty() ? "" : " ") + one.str();
    }
    option.FrameRatesSupported = fps_list;
    option.FPSEditable = profile.fps != 0 && !option.FrameRatesSupported.empty();
    option.bitrateEditable = profile.bitrate != 0 && min_rate != max_rate;
    option.resolutionEditable = option.ResolutionsAvailable.size() > 1;

    std::vector<std::string> codecs = splitOpt(child(video, "videoCodecType").attribute("opt").value());
    if (codecs.empty()) {
        codecs.push_back(text(video, "videoCodecType"));
    }
    profile.vEncoderOptionMap.clear();
    for (const auto &codec : codecs) {
        // PSIA does not tell the options per codec: every codec gets the channel's options.
        profile.vEncoderOptionMap[codecFromPsia(codec)] = option;
    }
    profile.videoEncEditable = profile.vEncoderOptionMap.size() > 1;
    profile.videoConfigEditable = profile.videoEncEditable || option.bitrateEditable || option.FPSEditable ||
                                  option.resolutionEditable;
}

static void setText(pugi::xml_node parent, const char *name, const std::string &value) {
    pugi::xml_node node = child(parent, name);
    if (!node) {
        return; // never add nodes the camera did not send: it may reject unknown ones
    }
    node.text().set(value.c_str());
}

bool applyVideoEncoderConfig(const std::string &channel_xml, const VideoEncoderConfig &config, std::string &out) {
    pugi::xml_document doc;
    if (!load(doc, channel_xml)) {
        return false;
    }
    pugi::xml_node video = find(root(doc), "Video");
    if (!video) {
        return false;
    }
    if (!config.vcodec.empty()) {
        setText(video, "videoCodecType", codecToPsia(config.vcodec));
    }
    if (config.width > 0 && config.height > 0) {
        setText(video, "videoResolutionWidth", std::to_string(config.width));
        setText(video, "videoResolutionHeight", std::to_string(config.height));
    }
    if (config.bitrate > 0) {
        bool vbr = text(video, "videoQualityControlType") == "VBR" || text(video, "videoQualityControlType") == "vbr";
        setText(video, vbr ? "vbrUpperCap" : "constantBitRate", std::to_string(config.bitrate));
    }
    if (config.fps > 0) {
        setText(video, "maxFrameRate", std::to_string(static_cast<int>(std::lround(config.fps * 100))));
    }
    std::ostringstream oss;
    doc.save(oss, "", pugi::format_raw);
    out = oss.str();
    return true;
}

bool parsePtzCapabilities(const std::string &xml, PtzSpace &space, OnvifPTZProfile &profile) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return false;
    }
    pugi::xml_node top = root(doc);
    space = PtzSpace();

    pugi::xml_node abs_pt = find(top, "AbsolutePanTiltPositionSpace");
    if (abs_pt && range(abs_pt, "XRange", space.absPanMin, space.absPanMax) &&
        range(abs_pt, "YRange", space.absTiltMin, space.absTiltMax)) {
        space.hasAbsolute = true;
        pugi::xml_node abs_zoom = find(top, "AbsoluteZoomPositionSpace");
        if (!abs_zoom || !range(abs_zoom, "ZRange", space.absZoomMin, space.absZoomMax)) {
            space.absZoomMin = space.absZoomMax = 0;
        }
    }

    auto symmetric = [](float lo, float hi) { return std::max(std::fabs(lo), std::fabs(hi)); };
    float lo = 0, hi = 0;
    pugi::xml_node rel_pt = find(top, "RelativePanTiltSpace");
    if (rel_pt && range(rel_pt, "XRange", lo, hi)) {
        space.hasRelative = true;
        space.relPanMax = symmetric(lo, hi);
        space.relTiltMax = range(rel_pt, "YRange", lo, hi) ? symmetric(lo, hi) : space.relPanMax;
        pugi::xml_node rel_zoom = find(top, "RelativeZoomSpace");
        space.relZoomMax = rel_zoom && range(rel_zoom, "ZRange", lo, hi) ? symmetric(lo, hi) : 0;
    }
    pugi::xml_node cons_pt = find(top, "ContinuousPanTiltSpace");
    if (cons_pt && range(cons_pt, "XRange", lo, hi)) {
        space.hasContinuous = true;
        space.consPanMax = symmetric(lo, hi);
        space.consTiltMax = range(cons_pt, "YRange", lo, hi) ? symmetric(lo, hi) : space.consPanMax;
        pugi::xml_node cons_zoom = find(top, "ContinuousZoomSpace");
        space.consZoomMax = cons_zoom && range(cons_zoom, "ZRange", lo, hi) ? symmetric(lo, hi) : 0;
    }

    // The controller works in ONVIF's normalized spaces; PsiaControl converts at the wire.
    profile.isAbsMoveEnable = space.hasAbsolute;
    profile.absMinPan = profile.absMinTilt = -1;
    profile.absMaxPan = profile.absMaxTilt = 1;
    profile.absMinZoom = 0;
    profile.absMaxZoom = 1;
    profile.isRelMoveEnable = space.hasRelative;
    profile.relMinPan = profile.relMinTilt = profile.relMinZoom = -1;
    profile.relMaxPan = profile.relMaxTilt = profile.relMaxZoom = 1;
    profile.isConsMoveEnable = space.hasContinuous;
    profile.consMinPan = profile.consMinTilt = profile.consMinZoom = -1;
    profile.consMaxPan = profile.consMaxTilt = profile.consMaxZoom = 1;
    return space.hasAbsolute || space.hasRelative || space.hasContinuous;
}

float toRaw(float normalized, float min, float max, bool unit) {
    float n = unit ? std::max(0.0f, std::min(1.0f, normalized)) : std::max(-1.0f, std::min(1.0f, normalized));
    float ratio = unit ? n : (n + 1) / 2;
    return min + ratio * (max - min);
}

float toNormalized(float raw, float min, float max, bool unit) {
    if (max <= min) {
        return 0;
    }
    float ratio = std::max(0.0f, std::min(1.0f, (raw - min) / (max - min)));
    return unit ? ratio : ratio * 2 - 1;
}

bool parsePtzStatus(const std::string &xml, const PtzSpace &space, float &pan, float &tilt, float &zoom) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return false;
    }
    pugi::xml_node abs = find(root(doc), "AbsoluteHigh");
    if (!abs) {
        return false;
    }
    pan = toNormalized(strtof(text(abs, "azimuth").c_str(), nullptr), space.absPanMin, space.absPanMax);
    tilt = toNormalized(strtof(text(abs, "elevation").c_str(), nullptr), space.absTiltMin, space.absTiltMax);
    zoom = toNormalized(strtof(text(abs, "absoluteZoom").c_str(), nullptr), space.absZoomMin, space.absZoomMax, true);
    return true;
}

bool parsePresets(const std::string &xml, OnvifPTZProfile::PTZPresetMap &out) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return false;
    }
    std::vector<pugi::xml_node> presets;
    findAll(root(doc), "PTZPreset", presets);
    out.clear();
    for (const auto &node : presets) {
        std::string id = text(node, "id");
        std::string enabled = text(node, "enabled");
        if (id.empty() || (!enabled.empty() && !isTrue(enabled))) {
            continue;
        }
        OnvifPTZProfile::PTZPreset preset;
        preset.Token = id;
        preset.Name = text(node, "presetName");
        if (preset.Name.empty()) {
            preset.Name = "Preset " + id;
        }
        out[id] = preset;
    }
    return true;
}

bool parseOutputs(const std::string &xml, std::vector<OnvifRelayOutputProfile> &out) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return false;
    }
    std::vector<pugi::xml_node> ports;
    findAll(root(doc), "IOOutputPort", ports);
    out.clear();
    for (const auto &node : ports) {
        OnvifRelayOutputProfile relay;
        relay.token = text(node, "id");
        if (relay.token.empty()) {
            continue;
        }
        relay.name = text(node, "name");
        if (relay.name.empty()) {
            relay.name = "Output " + relay.token;
        }
        out.push_back(relay);
    }
    return true;
}

std::string parseFirstPtzChannel(const std::string &xml) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return "";
    }
    std::vector<pugi::xml_node> channels;
    findAll(root(doc), "PTZChannel", channels);
    for (const auto &node : channels) {
        std::string enabled = text(node, "enabled");
        if (!enabled.empty() && !isTrue(enabled)) {
            continue;
        }
        std::string id = text(node, "id");
        if (!id.empty()) {
            return id;
        }
    }
    return "";
}

int parseRtspPort(const std::string &xml) {
    pugi::xml_document doc;
    if (!load(doc, xml)) {
        return 0;
    }
    std::vector<pugi::xml_node> entries;
    findAll(root(doc), "AdminAccessProtocol", entries);
    for (const auto &node : entries) {
        std::string enabled = text(node, "enabled");
        if (strcasecmp(text(node, "protocol").c_str(), "RTSP") != 0 || (!enabled.empty() && !isTrue(enabled))) {
            continue;
        }
        int port = atoi(text(node, "portNo").c_str());
        if (port > 0 && port < 65536) {
            return port;
        }
    }
    return 0;
}

ConnectError classifyConnectError(int http_status, int soap_error, bool is_device_info) {
    if (http_status == 0) {
        // No HTTP answer at all: refused, reset, timed out.
        return (soap_error == SOAP_TCP_ERROR || soap_error == SOAP_EOF || soap_error == SOAP_SSL_ERROR)
            ? ConnectError::UNREACHABLE : ConnectError::OTHER;
    }
    if (http_status == 401 || http_status == 403) {
        return ConnectError::AUTH_FAILED;
    }
    if (http_status == 404 || http_status == 405 || (http_status / 100 == 2 && !is_device_info)) {
        return ConnectError::NOT_THIS_PROTOCOL;
    }
    return http_status / 100 == 2 ? ConnectError::NONE : ConnectError::OTHER;
}

int posixTimeZoneOffset(const std::string &tz) {
    // "CST-7:00:00": the name, then the offset to ADD to local time to get UTC.
    size_t pos = tz.find_first_of("+-0123456789");
    if (pos == std::string::npos) {
        return 0;
    }
    int sign = 1;
    if (tz[pos] == '+' || tz[pos] == '-') {
        sign = tz[pos] == '-' ? -1 : 1;
        ++pos;
    }
    int h = 0, m = 0, s = 0;
    sscanf(tz.c_str() + pos, "%d:%d:%d", &h, &m, &s);
    return -sign * (h * 3600 + m * 60 + s);
}

static time_t utcFromTm(struct tm &tm) {
    return timegm(&tm);
}

bool parseLocalTime(const std::string &value, int default_offset, time_t &utc, bool &has_suffix) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int consumed = 0;
    if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &consumed) != 6) {
        return false;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    const char *rest = value.c_str() + consumed;
    while (*rest == '.' || isdigit(static_cast<unsigned char>(*rest))) {
        ++rest; // fractional seconds
    }
    int offset = default_offset;
    has_suffix = false;
    if (*rest == 'Z' || *rest == 'z') {
        offset = 0;
        has_suffix = true;
    } else if (*rest == '+' || *rest == '-') {
        int h = 0, m = 0;
        if (sscanf(rest + 1, "%d:%d", &h, &m) < 1) {
            return false;
        }
        offset = (*rest == '-' ? -1 : 1) * (h * 3600 + m * 60);
        has_suffix = true;
    }
    utc = utcFromTm(tm) - offset;
    return true;
}

bool applyManualTime(const std::string &time_xml, time_t now, std::string &out, time_t &camera_utc) {
    pugi::xml_document doc;
    if (!load(doc, time_xml)) {
        return false;
    }
    pugi::xml_node node = find(root(doc), "Time");
    if (!node) {
        return false;
    }
    std::string local_time = text(node, "localTime");
    int offset = posixTimeZoneOffset(text(node, "timeZone"));
    bool has_suffix = false;
    if (!parseLocalTime(local_time, offset, camera_utc, has_suffix)) {
        return false;
    }
    // Write the time back the way the camera wrote it: its own offset, with or without a suffix.
    time_t shifted = now + offset;
    struct tm tm;
    gmtime_r(&shifted, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::string value = buf;
    if (has_suffix) {
        if (offset == 0 && local_time.find_first_of("Zz") != std::string::npos) {
            value += "Z";
        } else {
            int abs_offset = offset < 0 ? -offset : offset;
            snprintf(buf, sizeof(buf), "%c%02d:%02d", offset < 0 ? '-' : '+', abs_offset / 3600, abs_offset % 3600 / 60);
            value += buf;
        }
    }
    setText(node, "timeMode", "manual");
    setText(node, "localTime", value);
    std::ostringstream oss;
    doc.save(oss, "", pugi::format_raw);
    out = oss.str();
    return true;
}

} // namespace psia_detail

using namespace psia_detail;

const HttpXmlDialect &HttpXmlDialect::psia() {
    static const HttpXmlDialect dialect = {
        "PSIA",
        &PsiaCtrl::kRequestTimeoutMs,
        "/PSIA/System/deviceInfo",
        "/PSIA/Streaming/channels",
        "/PSIA/PTZ/channels",
        "/PSIA/System/Video/inputs/channels",
        "/PSIA/IO/outputs",
        "/PSIA/System/time",
        "/PSIA/Streaming/channels/",
        "",
    };
    return dialect;
}

const HttpXmlDialect &HttpXmlDialect::isapi() {
    static const HttpXmlDialect dialect = {
        "ISAPI",
        &IsapiCtrl::kRequestTimeoutMs,
        "/ISAPI/System/deviceInfo",
        "/ISAPI/Streaming/channels",
        "/ISAPI/PTZCtrl/channels",
        "/ISAPI/System/Video/inputs/channels",
        "/ISAPI/System/IO/outputs",
        "/ISAPI/System/time",
        "/Streaming/Channels/",
        // Hikvision's DeviceInfo has no manufacturer, and deviceName is whatever the user named it.
        "Hikvision",
    };
    return dialect;
}

// ── PsiaControl ───────────────────────────────────────────────────────────────────────────────────

PsiaControl::PsiaControl(std::string address, std::string username, std::string password)
    : PsiaControl(HttpXmlDialect::psia(), std::move(address), std::move(username), std::move(password)) {}

PsiaControl::PsiaControl(const HttpXmlDialect &dialect, std::string address, std::string username, std::string password)
    : _dialect(dialect), _address(std::move(address)), _username(std::move(username)), _password(std::move(password)) {
    _host = _address.substr(0, _address.rfind(':') == std::string::npos ? _address.size() : _address.rfind(':'));
}

PsiaControl::~PsiaControl() {
    disconnect();
}

void PsiaControl::setError(const std::string &err) {
    std::lock_guard<std::mutex> lck(_state_mtx);
    _lastError = err;
}

std::string PsiaControl::lastError() {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _lastError;
}

int PsiaControl::request(const char *method, const std::string &path, const std::string &body, std::string &response) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    response.clear();
    _lastStatus = 0;
    _lastSoapError = 0;
    if (!_soap) {
        _soap = soap_new();
        int timeout_ms = mINI::Instance()[*_dialect.timeoutKey].as<int>();
        // gSOAP takes negative values as microseconds.
        int timeout = timeout_ms > 0 ? -timeout_ms * 1000 : -5000000;
        _soap->connect_timeout = _soap->recv_timeout = _soap->send_timeout = timeout;
        soap_register_plugin(_soap, http_da);
    }
    const std::string url = "http://" + _address + path;
    const bool is_get = strcmp(method, "GET") == 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Basic works on its own; the Digest plugin needs the challenge saved below.
        _soap->userid = _username.c_str();
        _soap->passwd = _password.c_str();
        if (_digest) {
            http_da_restore(_soap, _digest);
        }
        int err = is_get ? soap_GET(_soap, url.c_str(), nullptr)
                         : soap_PUT(_soap, url.c_str(), nullptr, "application/xml");
        if (!err && !is_get) {
            err = soap_send(_soap, body.c_str()) || soap_end_send(_soap);
            if (err) {
                err = _soap->error;
            }
        }
        _soap->http_content = nullptr;
        char *data = nullptr;
        size_t len = 0;
        if (!err) {
            err = soap_begin_recv(_soap);
        }
        if (!err) {
            data = soap_http_get_body(_soap, &len);
            err = data ? soap_end_recv(_soap) : _soap->error;
        }
        // 2xx/400/500 come back as err == 0 with the status in soap->status; the other HTTP
        // statuses come back as the error itself.
        int status = 0;
        if (!err) {
            status = _soap->status;
        } else if (err >= 100 && err < 600) {
            status = err;
        }
        if (data && len) {
            response.assign(data, len);
        }
        const bool challenged = status == 401 && _soap->authrealm && attempt == 0;
        if (challenged) {
            if (_digest) {
                http_da_release(_soap, _digest);
            } else {
                _digest = new http_da_info();
            }
            http_da_save(_soap, _digest, _soap->authrealm, _username.c_str(), _password.c_str());
        }
        _lastStatus = status;
        _lastSoapError = status ? 0 : err;
        soap_closesock(_soap);
        soap_destroy(_soap);
        soap_end(_soap);
        if (challenged) {
            continue;
        }
        if (status == 0) {
            setError(std::string(_dialect.name) + " " + method + " " + path + " failed: soap error " + std::to_string(err));
        } else if (status / 100 != 2) {
            ResponseStatus rs;
            std::string err_msg = std::string(_dialect.name) + " " + method + " " + path + ": HTTP " + std::to_string(status);
            if (parseResponseStatus(response, rs)) {
                err_msg += " " + rs.statusString + " (" + rs.subStatusCode + ")";
            }
            setError(err_msg);
        }
        return status;
    }
    return _lastStatus;
}

bool PsiaControl::get(const std::string &path, std::string &response) {
    return request("GET", path, "", response) / 100 == 2;
}

bool PsiaControl::put(const std::string &path, const std::string &body) {
    std::string response;
    if (request("PUT", path, body, response) / 100 != 2) {
        return false;
    }
    ResponseStatus rs;
    if (parseResponseStatus(response, rs) && !isResponseStatusOk(rs)) {
        setError(std::string(_dialect.name) + " PUT " + path + ": " + rs.statusString + " (" + rs.subStatusCode + ")");
        return false;
    }
    return true;
}

bool PsiaControl::readDeviceInfo(OnvifDeviceInfo &info) {
    std::string xml;
    if (!get(_dialect.deviceInfo, xml)) {
        return false;
    }
    if (!parseDeviceInfo(xml, info, _dialect.defaultManufacturer)) {
        setError(std::string(_dialect.name) + " " + _dialect.deviceInfo + ": the answer is not a DeviceInfo document");
        return false;
    }
    return true;
}

bool PsiaControl::readProfiles(OnvifMediaProfileMap &profiles) {
    std::string xml;
    if (!get(_dialect.channels, xml)) {
        return false;
    }
    if (!parseStreamingChannels(xml, _host, profiles, _dialect.rtspPath, defaultRtspPort())) {
        setError(std::string(_dialect.name) + " " + _dialect.channels + ": the answer is not a StreamingChannelList");
        return false;
    }
    for (auto &profile : profiles) {
        std::string caps;
        if (get(std::string(_dialect.channels) + "/" + profile.token + "/capabilities", caps)) {
            parseChannelCapabilities(caps, profile);
        }
    }
    return true;
}

void PsiaControl::readPtz() {
    std::string xml;
    std::string channel;
    const std::string ptz_root = _dialect.ptzChannels;
    if (get(ptz_root, xml)) {
        channel = parseFirstPtzChannel(xml);
    }
    PtzSpace space;
    OnvifPTZProfile profile;
    OnvifImageProfile image;
    if (!channel.empty()) {
        std::string caps;
        if (get(ptz_root + "/" + channel + "/capabilities", caps)) {
            parsePtzCapabilities(caps, space, profile);
        }
        std::string presets;
        if (get(ptz_root + "/" + channel + "/presets", presets) && parsePresets(presets, profile.presetMap)) {
            profile.isPresetEnable = true;
        }
        profile.isHomePresetEnable = profile.isAbsMoveEnable || profile.isRelMoveEnable || profile.isConsMoveEnable;
        // PSIA/ISAPI have no capability flag for the lens: a camera with a PTZ channel gets focus and iris,
        // and a camera that rejects the command reports the error then.
        image.isFocusConsEnable = image.isFocusEnable = true;
        image.isIrisEnable = true;
        image.videoSourceToken = _videoInput;
    }
    std::lock_guard<std::mutex> lck(_state_mtx);
    _ptzChannel = channel;
    _ptzSpace = space;
    profile.strMediaProfileToken = _profiles.empty() ? "" : _profiles.front().token;
    _ptzProfile = profile;
    _imageProfile = image;
}

void PsiaControl::readOutputs() {
    std::string xml;
    std::vector<OnvifRelayOutputProfile> outputs;
    if (get(_dialect.outputs, xml)) {
        parseOutputs(xml, outputs);
    }
    std::lock_guard<std::mutex> lck(_state_mtx);
    _outputs = outputs;
}

bool PsiaControl::connect() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    _connected = false;
    OnvifDeviceInfo info;
    if (!readDeviceInfo(info)) {
        _lastConnectError = classifyConnectError(_lastStatus, _lastSoapError, false);
        WarnL << _dialect.name << " connect to " << _address << " failed (" << connectErrorToString(_lastConnectError.load())
              << "): " << lastError();
        disconnect();
        return false;
    }
    OnvifMediaProfileMap profiles;
    if (!readProfiles(profiles)) {
        _lastConnectError = ConnectError::OTHER;
        WarnL << _dialect.name << " connect to " << _address << " failed reading the streaming channels: " << lastError();
        disconnect();
        return false;
    }
    {
        std::lock_guard<std::mutex> lck(_state_mtx);
        _deviceInfo = info;
        _profiles = profiles;
    }
    readPtz();
    readOutputs();
    _lastConnectError = ConnectError::NONE;
    _connected = true;
    InfoL << _dialect.name << " device " << _address << ": " << info.manufacturer << " " << info.model << ", "
          << profiles.size() << " streaming channel(s)";
    return true;
}

void PsiaControl::disconnect() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    _connected = false;
    if (_soap) {
        if (_digest) {
            http_da_release(_soap, _digest);
        }
        soap_destroy(_soap);
        soap_end(_soap);
        soap_free(_soap);
        _soap = nullptr;
    }
    delete _digest;
    _digest = nullptr;
}

bool PsiaControl::probeConnection() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (!_connected) {
        return false;
    }
    OnvifDeviceInfo info;
    if (!readDeviceInfo(info)) {
        return false;
    }
    if (info != getDeviceInfo()) {
        WarnL << _dialect.name << " device " << _address << " reports a different identity than the one we connected to";
        return false;
    }
    return true;
}

OnvifDeviceInfo PsiaControl::getDeviceInfo() {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _deviceInfo;
}

OnvifMediaProfileMap PsiaControl::getMediaProfilesInfo() {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _profiles;
}

bool PsiaControl::refreshMediaProfiles() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (!_connected) {
        return false;
    }
    OnvifMediaProfileMap profiles;
    if (!readProfiles(profiles)) {
        return false;
    }
    std::lock_guard<std::mutex> lck(_state_mtx);
    _profiles = profiles;
    return true;
}

bool PsiaControl::getVideoEncoderConfigByToken(const std::string &token, VideoEncoderConfig &config) {
    std::string xml;
    if (!get(std::string(_dialect.channels) + "/" + token, xml)) {
        return false;
    }
    OnvifMediaProfileMap profiles;
    if (!parseStreamingChannels(xml, _host, profiles) || profiles.empty()) {
        setError(std::string(_dialect.name) + " streaming channel " + token + " has no video");
        return false;
    }
    const auto &profile = profiles.front();
    config.vcodec = profile.vcodec;
    config.width = profile.width;
    config.height = profile.height;
    config.bitrate = profile.bitrate;
    config.fps = profile.fps;
    config.quality = profile.quality;
    return true;
}

bool PsiaControl::setVideoEncoderConfigByToken(const std::string &token, const VideoEncoderConfig &config) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    const std::string path = std::string(_dialect.channels) + "/" + token;
    std::string xml, updated;
    if (!get(path, xml)) {
        return false;
    }
    if (!applyVideoEncoderConfig(xml, config, updated)) {
        setError(std::string(_dialect.name) + " streaming channel " + token + " has no Video node");
        return false;
    }
    return put(path, updated);
}

void PsiaControl::setCameraTimeManual() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    std::string xml, updated;
    if (!get(_dialect.time, xml)) {
        return;
    }
    time_t now = time(nullptr);
    time_t camera_utc = 0;
    if (!applyManualTime(xml, now, updated, camera_utc)) {
        WarnL << _dialect.name << " device " << _address << ": cannot read the camera time";
        return;
    }
    if (std::llabs(static_cast<long long>(camera_utc - now)) < 10) {
        return;
    }
    if (!put(_dialect.time, updated)) {
        WarnL << _dialect.name << " device " << _address << ": set camera time failed: " << lastError();
    }
}

// ── PTZ ─────────────────────────────────────────────────────────────────────────────────────────

OnvifPTZProfile PsiaControl::getPTZProfile() {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _ptzProfile;
}

bool PsiaControl::enablePTZ() {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _ptzProfile.isAbsMoveEnable || _ptzProfile.isConsMoveEnable || _ptzProfile.isRelMoveEnable;
}

bool PsiaControl::sendPtz(const std::string &sub, const std::string &body) {
    std::string channel;
    {
        std::lock_guard<std::mutex> lck(_state_mtx);
        channel = _ptzChannel;
    }
    if (channel.empty()) {
        setError(std::string(_dialect.name) + " device has no PTZ channel");
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    return put(std::string(_dialect.ptzChannels) + "/" + channel + "/" + sub, body);
}

static std::string num(float v) {
    return std::to_string(static_cast<int>(std::lround(v)));
}

bool PsiaControl::PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout) {
    PtzSpace space;
    {
        std::lock_guard<std::mutex> lck(_state_mtx);
        space = _ptzSpace;
    }
    auto clamp1 = [](float v) { return std::max(-1.0f, std::min(1.0f, v)); };
    return sendPtz("continuous", "<PTZData><pan>" + num(clamp1(pan) * space.consPanMax) + "</pan><tilt>" +
                                     num(clamp1(tilt) * space.consTiltMax) + "</tilt><zoom>" +
                                     num(clamp1(zoom) * space.consZoomMax) + "</zoom></PTZData>");
}

bool PsiaControl::PTZ_Stop(bool panTilt, bool zoom) {
    return sendPtz("continuous", "<PTZData><pan>0</pan><tilt>0</tilt><zoom>0</zoom></PTZData>");
}

bool PsiaControl::PTZ_RelativeMove(float pan, float tilt, float zoom, float, float, float) {
    PtzSpace space;
    {
        std::lock_guard<std::mutex> lck(_state_mtx);
        space = _ptzSpace;
    }
    auto clamp1 = [](float v) { return std::max(-1.0f, std::min(1.0f, v)); };
    return sendPtz("relative", "<PTZData><Relative><positionX>" + num(clamp1(pan) * space.relPanMax) +
                                   "</positionX><positionY>" + num(clamp1(tilt) * space.relTiltMax) +
                                   "</positionY><relativeZoom>" + num(clamp1(zoom) * space.relZoomMax) +
                                   "</relativeZoom></Relative></PTZData>");
}

bool PsiaControl::PTZ_AbsoluteMove(float pan, float tilt, float zoom) {
    PtzSpace space;
    {
        std::lock_guard<std::mutex> lck(_state_mtx);
        space = _ptzSpace;
    }
    return sendPtz("absolute", "<PTZData><AbsoluteHigh><elevation>" +
                                   num(toRaw(tilt, space.absTiltMin, space.absTiltMax)) + "</elevation><azimuth>" +
                                   num(toRaw(pan, space.absPanMin, space.absPanMax)) + "</azimuth><absoluteZoom>" +
                                   num(toRaw(zoom, space.absZoomMin, space.absZoomMax, true)) +
                                   "</absoluteZoom></AbsoluteHigh></PTZData>");
}

bool PsiaControl::PTZ_AbsoluteMove(float pan, float tilt, float zoom, float, float, float) {
    // PSIA absolute moves have no speed.
    return PTZ_AbsoluteMove(pan, tilt, zoom);
}

bool PsiaControl::PTZ_GotoPreset(const std::string &presetToken, float, float, float) {
    return sendPtz("presets/" + presetToken + "/goto", "");
}

bool PsiaControl::PTZ_GotoHomePosition(float, float, float) {
    return sendPtz("homeposition/goto", "");
}

PtzMoveStatus PsiaControl::PTZ_GetStatus(float &pan, float &tilt, float &zoom) {
    std::string channel;
    PtzSpace space;
    {
        std::lock_guard<std::mutex> lck(_state_mtx);
        channel = _ptzChannel;
        space = _ptzSpace;
    }
    if (channel.empty()) {
        setError(std::string(_dialect.name) + " device has no PTZ channel");
        return PtzMoveStatus::UNKNOWN;
    }
    std::string xml;
    if (!get(std::string(_dialect.ptzChannels) + "/" + channel + "/status", xml)) {
        return PtzMoveStatus::UNKNOWN;
    }
    if (!parsePtzStatus(xml, space, pan, tilt, zoom)) {
        setError(std::string(_dialect.name) + " PTZ status has no absolute position");
        return PtzMoveStatus::UNKNOWN;
    }
    // PSIA reports no movement state: a readable position is as good as ONVIF's IDLE.
    return PtzMoveStatus::IDLE;
}

bool PsiaControl::PTZ_AuxiliaryCommand(const std::string &auxiliaryData) {
    int direction = auxiliaryData == "tt:Iris|open" ? 1 : auxiliaryData == "tt:Iris|close" ? -1 : 0;
    if (direction == 0) {
        setError(std::string(_dialect.name) + " does not support auxiliary command: " + auxiliaryData);
        return false;
    }
    // ONVIF's iris auxiliary command is one step; PSIA moves the iris until told to stop.
    const std::string path = std::string(_dialect.videoInputs) + "/" + _videoInput + "/iris";
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    if (!put(path, "<IrisData><iris>" + std::to_string(direction * 50) + "</iris></IrisData>")) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return put(path, "<IrisData><iris>0</iris></IrisData>");
}

// ── imaging / relay ─────────────────────────────────────────────────────────────────────────────

OnvifImageProfile PsiaControl::getImageProfile() const {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _imageProfile;
}

bool PsiaControl::enableFocus() {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _imageProfile.isFocusEnable;
}

bool PsiaControl::enableIris() {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _imageProfile.isIrisEnable;
}

bool PsiaControl::Imaging_FocusMove(float speed) {
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    int value = static_cast<int>(std::lround(std::max(-1.0f, std::min(1.0f, speed)) * 100));
    return put(std::string(_dialect.videoInputs) + "/" + _videoInput + "/focus",
               "<FocusData><focus>" + std::to_string(value) + "</focus></FocusData>");
}

bool PsiaControl::Imaging_FocusStop() {
    return put(std::string(_dialect.videoInputs) + "/" + _videoInput + "/focus", "<FocusData><focus>0</focus></FocusData>");
}

std::vector<OnvifRelayOutputProfile> PsiaControl::getRelayOutputProfiles() const {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return _outputs;
}

bool PsiaControl::enableRelayOutput() {
    std::lock_guard<std::mutex> lck(_state_mtx);
    return !_outputs.empty();
}

bool PsiaControl::Relay_SetOutputState(const std::string &relayToken, bool active) {
    {
        std::lock_guard<std::mutex> lck(_state_mtx);
        auto it = std::find_if(_outputs.begin(), _outputs.end(),
                               [&](const OnvifRelayOutputProfile &r) { return r.token == relayToken; });
        if (it == _outputs.end()) {
            _lastError = "Relay token not found: " + relayToken;
            return false;
        }
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    if (!put(std::string(_dialect.outputs) + "/" + relayToken + "/trigger",
             std::string("<IOPortData><outputState>") + (active ? "high" : "low") + "</outputState></IOPortData>")) {
        return false;
    }
    std::lock_guard<std::mutex> lck(_state_mtx);
    for (auto &relay : _outputs) {
        if (relay.token == relayToken) {
            relay.isActive = active;
        }
    }
    return true;
}

// ── IsapiControl ──────────────────────────────────────────────────────────────────────────────────

IsapiControl::IsapiControl(std::string address, std::string username, std::string password)
    : PsiaControl(HttpXmlDialect::isapi(), std::move(address), std::move(username), std::move(password)) {}

int IsapiControl::defaultRtspPort() {
    std::string xml;
    int port = get("/ISAPI/Security/adminAccesses", xml) ? parseRtspPort(xml) : 0;
    if (port <= 0) {
        DebugL << "ISAPI device " << _address << ": no RTSP port in adminAccesses, using 554";
        return 554;
    }
    return port;
}

} // namespace managerkit
