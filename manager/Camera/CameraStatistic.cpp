#include "CameraStatistic.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "Local/TierStorageManager.h"
#include "Extension/Resource.h"
#include "Common/BlockingExecutors.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

namespace {

std::string normalizeReduceQualityCodec(const std::string &codec) {
    std::string value(codec);
    std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
        return static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
    });
    if (value == "h264" || value == "avc") return "H264";
    if (value == "h265" || value == "hevc") return "H265";
    return "";
}

bool higherReduceQuality(const Broadcast::ReduceQualityProfile &a,
                         const Broadcast::ReduceQualityProfile &b) {
    const long long pixels_a = static_cast<long long>(a.width) * a.height;
    const long long pixels_b = static_cast<long long>(b.width) * b.height;
    if (pixels_a != pixels_b) return pixels_a > pixels_b;
    if (a.bitrate != b.bitrate) return a.bitrate > b.bitrate;
    if (a.fps != b.fps) return a.fps > b.fps;
    if (a.source_stream_id != b.source_stream_id) return a.source_stream_id < b.source_stream_id;
    return a.codec < b.codec;
}

} // namespace

ReduceQualityCapability ReduceQualityCapabilityBuilder::build(
    const CameraOption &option,
    const std::unordered_map<int, StreamTuple> &stream_map,
    const std::unordered_map<int, StreamStatistic> &statistics) {
    ReduceQualityCapability result;
#if !defined(ENABLE_FFMPEG)
    (void)option;
    (void)stream_map;
    (void)statistics;
    return result;
#else
    result.supported = option.enableReduceQualityOnView;
    if (!result.supported) return result;
    static const int ladder[][4] = {
        {1920, 1080, 4000000, 25}, {1280, 720, 2000000, 20},
        {854, 480, 1000000, 15}, {640, 360, 512000, 15}, {426, 240, 256000, 10}
    };
    std::set<std::string> seen;
    for (const auto &entry : ladder) {
        const StreamTuple *best_tuple = nullptr;
        const StreamStatistic *best = nullptr;
        long long best_pixels = 0;
        for (const auto &pair : statistics) {
            auto tuple_it = stream_map.find(pair.first);
            const auto &stat = pair.second;
            if (tuple_it == stream_map.end() || !stat.live || !stat.has_video ||
                stat.width <= 0 || stat.height <= 0 ||
                normalizeReduceQualityCodec(stat.vcodec).empty()) continue;
            const double scale = std::min(static_cast<double>(entry[0]) / stat.width,
                                          static_cast<double>(entry[1]) / stat.height);
            if (scale >= 1.0) continue;
            const long long pixels = static_cast<long long>(stat.width) * stat.height;
            if (!best || pixels < best_pixels) {
                best = &stat; best_tuple = &tuple_it->second; best_pixels = pixels;
            }
        }
        if (!best || !best_tuple) continue;
        const double scale = std::min(static_cast<double>(entry[0]) / best->width,
                                      static_cast<double>(entry[1]) / best->height);
        Broadcast::ReduceQualityProfile profile;
        profile.source_stream_id = best_tuple->stream_id;
        profile.codec = normalizeReduceQualityCodec(best->vcodec);
        profile.width = std::max(2, static_cast<int>(std::floor(best->width * scale)) & ~1);
        profile.height = std::max(2, static_cast<int>(std::floor(best->height * scale)) & ~1);
        profile.bitrate = best->bitrate > 0 ? std::min(best->bitrate, entry[2]) : entry[2];
        profile.fps = best->fps > 0
            ? std::min(static_cast<int>(std::floor(best->fps)), entry[3])
            : entry[3];
        const std::string key = profile.source_stream_id + "|" + profile.codec + "|" +
            std::to_string(profile.width) + "|" + std::to_string(profile.height) + "|" +
            std::to_string(profile.bitrate) + "|" + std::to_string(profile.fps);
        if (profile.fps > 0 && profile.bitrate > 0 && seen.emplace(key).second) {
            result.profiles.emplace_back(profile);
        }
    }
    std::sort(result.profiles.begin(), result.profiles.end(), higherReduceQuality);
    return result;
#endif
}

Json::Value ReduceQualityCapabilityBuilder::toJson(const ReduceQualityCapability &capability) {
    Json::Value json(Json::objectValue);
    json["supported"] = capability.supported;
    json["profiles"] = Json::arrayValue;
    for (const auto &profile : capability.profiles) {
        Json::Value item(Json::objectValue);
        item["sourceStreamId"] = profile.source_stream_id;
        item["codec"] = profile.codec;
        item["width"] = profile.width;
        item["height"] = profile.height;
        item["bitrate"] = profile.bitrate;
        item["fps"] = profile.fps;
        json["profiles"].append(item);
    }
    return json;
}


static Json::Value makeJsonKeyValue(const string &key, const string &value) {
    Json::Value ret;
    ret["name"] = key;
    ret["value"] = value;
    return ret;
}

// BookmarkStats
static Json::Value makeBookmarkStatsJson(BookmarkStats stats) {
    Json::Value ret;
    ret["recordAverageSizeB"] = stats.recordAverageSizeB;
    ret["recordCount"] = stats.recordCount;
    return ret;
}

static BookmarkStats getBookmarkStats(const Json::Value &data) {
    BookmarkStats stats;
    stats.recordAverageSizeB = data["recordAverageSizeB"].asUInt64();
    stats.recordCount = data["recordCount"].asUInt64();
    return stats;
}

// StreamStorageStats
static Json::Value makeStreamStorageStatsJson(const unordered_map<string, StreamStorageStats> storage_map, const unordered_map<int, StreamTuple> stream_map, int stream_type) {
    Json::Value ret = Json::objectValue;
    auto it_stream = stream_map.find(stream_type);
    if (it_stream != stream_map.end()) {
        auto stream_id = it_stream->second.stream_id;
        auto it_storage = storage_map.find(stream_id);
        if (it_storage != storage_map.end()) {
            ret["archiveIndexRecordCount"] = it_storage->second.archiveIndexRecordCount;
            ret["archiveSizeB"] = it_storage->second.archiveSizeB;
            ret["archiveStartTime"] = it_storage->second.archiveStartTime;
            ret["archiveEndTime"] = it_storage->second.archiveEndTime;
        }
    }
    return ret;
}

static StreamStorageStats getStreamStorageStats(const Json::Value &data) {
    StreamStorageStats stats;
    stats.archiveIndexRecordCount = data["archiveIndexRecordCount"].asUInt64();
    stats.archiveSizeB = data["archiveSizeB"].asUInt64();
    stats.archiveStartTime = data["archiveStartTime"].asUInt64();
    stats.archiveEndTime = data["archiveEndTime"].asUInt64();
    return stats;
}

// StreamStatistic
static Json::Value makeStreamStatisticJson(const unordered_map<int, StreamStatistic> stats_map, int stream_type) {
    Json::Value ret = Json::objectValue;
    auto it_statistic = stats_map.find(stream_type);
    if (it_statistic != stats_map.end()) {
        ret["live"] = it_statistic->second.live;
        ret["last_change_status"] = it_statistic->second.last_change_status;
        ret["status"] = it_statistic->second.status;
        ret["byte_speed"] = it_statistic->second.byte_speed;
        ret["has_video"] = it_statistic->second.has_video;
        ret["vcodec"] = it_statistic->second.vcodec;
        ret["source_vcodec"] = it_statistic->second.source_vcodec;
        ret["transcoded"] = it_statistic->second.transcoded;
        ret["width"] = it_statistic->second.width;
        ret["height"] = it_statistic->second.height;
        ret["bitrate"] = it_statistic->second.bitrate;
        ret["fps"] = it_statistic->second.fps;
        ret["has_audio"] = it_statistic->second.has_audio;
        ret["acodec"] = it_statistic->second.acodec;
        ret["sample_rate"] = it_statistic->second.sample_rate;
        ret["channel_no"] = it_statistic->second.channel_no;
        ret["sample_bit"] = it_statistic->second.sample_bit;
    }
    return ret;
}

static StreamStatistic getStreamStatistic(const Json::Value &data) {
    StreamStatistic stats;
    stats.live = data["live"].asBool();
    stats.last_change_status = data["last_change_status"].asUInt64();
    stats.status = data["status"].asString();
    stats.byte_speed = data["byte_speed"].asInt();
    stats.has_video = data["has_video"].asBool();
    stats.vcodec = data["vcodec"].asString();
    stats.source_vcodec = data.isMember("source_vcodec") ? data["source_vcodec"].asString() : stats.vcodec;
    stats.transcoded = data["transcoded"].asBool();
    stats.width = data["width"].asInt();
    stats.height = data["height"].asInt();
    stats.bitrate = data["bitrate"].asInt();
    stats.fps = data["fps"].asFloat();
    stats.has_audio = data["has_audio"].asBool();
    stats.acodec = data["acodec"].asString();
    stats.sample_rate = data["sample_rate"].asInt();
    stats.channel_no = data["channel_no"].asInt();
    stats.sample_bit = data["sample_bit"].asInt();
    return stats;
}

static Json::Value makeTransportStatsJson(const TransportStats &stats) {
    Json::Value ret = Json::objectValue;
    ret["liveTransports"] = Json::arrayValue;
    for (const auto &transport : stats.liveTransports) {
        ret["liveTransports"].append(transport);
    }
    ret["replayTransports"] = Json::arrayValue;
    for (const auto &transport : stats.replayTransports) {
        ret["replayTransports"].append(transport);
    }
    return ret;
}

static TransportStats getTransportStats(const Json::Value &data) {
    TransportStats stats;
    for (const auto &transport : data["liveTransports"]) {
        stats.liveTransports.push_back(transport.asString());
    }
    for (const auto &transport : data["replayTransports"]) {
        stats.replayTransports.push_back(transport.asString());
    }
    return stats;
}

// DeviceCapabilities
static Json::Value makeOnvifPTZPresetMapJson(const OnvifPTZProfile::PTZPresetMap &preset_map) {
    Json::Value ret = Json::arrayValue;
    for (const auto &it : preset_map) {
        auto preset = it.second;
        Json::Value p_json = Json::objectValue;
        p_json["Token"] = preset.Token;
        p_json["Name"] = preset.Name;
        p_json["Pan"] = preset.absPan;
        p_json["Tilt"] = preset.absTilt;
        p_json["Zoom"] = preset.absZoom;
        ret.append(p_json);
    }
    return ret;
}

static Json::Value makeOnvifProfileJson(const OnvifProfile &profile) {
    Json::Value ret = Json::objectValue;
    // mediaProfiles
    Json::Value mediaProfiles = Json::arrayValue;
    for (const auto &mp : profile.mediaProfiles) {
        Json::Value mp_json = Json::objectValue;
        mp_json["token"] = mp.token;
        mp_json["url"] = mp.url;
        mp_json["vcodec"] = mp.vcodec;
        mp_json["width"] = mp.width;
        mp_json["height"] = mp.height;
        mp_json["bitrate"] = mp.bitrate;
        mp_json["fps"] = mp.fps;
        for (const auto &vo : mp.vEncoderOptionMap) {
            mp_json["videoEncoder"]["available"][vo.first]["fps"]["supported"] = vo.second.FrameRatesSupported;
            mp_json["videoEncoder"]["available"][vo.first]["fps"]["editable"] = vo.second.FPSEditable;
            mp_json["videoEncoder"]["available"][vo.first]["bitrate"]["supported"] = vo.second.BitRateRange;
            mp_json["videoEncoder"]["available"][vo.first]["bitrate"]["editable"] = vo.second.bitrateEditable;
            mp_json["videoEncoder"]["available"][vo.first]["ResolutionsAvailable"]["supported"] = Json::arrayValue;
            for (const auto &r : vo.second.ResolutionsAvailable) {
                Json::Value stream;
                stream["width"] = r.first;
                stream["height"] = r.second;
                mp_json["videoEncoder"]["available"][vo.first]["ResolutionsAvailable"]["supported"] .append(stream);
            }
            mp_json["videoEncoder"]["available"][vo.first]["ResolutionsAvailable"]["editable"] = vo.second.resolutionEditable;
        }
        mp_json["videoEncEditable"] = mp.videoEncEditable;
        mp_json["videoConfigEditable"] = mp.videoConfigEditable;
        mediaProfiles.append(mp_json);
    }
    ret["mediaProfiles"] = mediaProfiles;
    // ptzProfile
    Json::Value ptzProfile = Json::objectValue;
    ptzProfile["strMediaProfileToken"] = profile.ptzProfile.strMediaProfileToken;
    ptzProfile["isAbsMoveEnable"] = profile.ptzProfile.isAbsMoveEnable;
    ptzProfile["isConsMoveEnable"] = profile.ptzProfile.isConsMoveEnable;
    ptzProfile["isRelMoveEnable"] = profile.ptzProfile.isRelMoveEnable;
    ptzProfile["isPresetEnable"] = profile.ptzProfile.isPresetEnable;
    ptzProfile["presetMap"] = makeOnvifPTZPresetMapJson(profile.ptzProfile.presetMap);
    ptzProfile["isHomePresetEnable"] = profile.ptzProfile.isHomePresetEnable;
    ptzProfile["homePresetToken"] = profile.ptzProfile.homePresetToken;
    ret["ptzProfile"] = ptzProfile;
    // deviceInfo
    Json::Value deviceInfo = Json::objectValue;
    deviceInfo["manufacturer"] = profile.deviceInfo.manufacturer;
    deviceInfo["model"] = profile.deviceInfo.model;
    deviceInfo["firmwareVersion"] = profile.deviceInfo.firmwareVersion;
    deviceInfo["serialNumber"] = profile.deviceInfo.serialNumber;
    deviceInfo["hardwareId"] = profile.deviceInfo.hardwareId;
    deviceInfo["macAddress"] = profile.deviceInfo.macAddress;
    ret["deviceInfo"] = deviceInfo;
    // Structured profiles
    Json::Value imageProfile = Json::objectValue;
    imageProfile["isFocusEnable"]       = profile.imageProfile.isFocusEnable;
    imageProfile["isFocusAbsEnable"]    = profile.imageProfile.isFocusAbsEnable;
    imageProfile["isFocusConsEnable"]   = profile.imageProfile.isFocusConsEnable;
    imageProfile["isFocusRelEnable"]    = profile.imageProfile.isFocusRelEnable;
    imageProfile["isFocusAutoSupported"]= profile.imageProfile.isFocusAutoSupported;
    imageProfile["isIrisEnable"]        = profile.imageProfile.isIrisEnable;
    imageProfile["isIrisAutoSupported"] = profile.imageProfile.isIrisAutoSupported;
    imageProfile["irisMin"]             = profile.imageProfile.irisMin;
    imageProfile["irisMax"]             = profile.imageProfile.irisMax;
    ret["imageProfile"] = imageProfile;
    Json::Value audioOutput = Json::objectValue;
    audioOutput["enable"]       = profile.audioOutputProfile.enable;
    audioOutput["token"]        = profile.audioOutputProfile.token;
    audioOutput["name"]         = profile.audioOutputProfile.name;
    audioOutput["outputToken"]  = profile.audioOutputProfile.outputToken;
    audioOutput["profileToken"] = profile.audioOutputProfile.profileToken;
    audioOutput["outputLevel"]  = profile.audioOutputProfile.outputLevel;
    audioOutput["sendPrimacy"]  = profile.audioOutputProfile.sendPrimacy;
    audioOutput["decoderToken"] = profile.audioOutputProfile.decoderToken;
    Json::Value supportedArray  = Json::arrayValue;
    for (const auto &it : profile.audioOutputProfile.supportedDecodingEncodings) {
        supportedArray.append(it);
    }
    audioOutput["supportedDecodingEncodings"] = supportedArray;
    ret["audioOutputProfile"] = audioOutput;
    Json::Value audioInput = Json::objectValue;
    audioInput["enable"]        = profile.audioInputProfile.enable;
    audioInput["token"]         = profile.audioInputProfile.token;
    audioInput["name"]          = profile.audioInputProfile.name;
    audioInput["sourceToken"]   = profile.audioInputProfile.sourceToken;
    audioInput["encoderToken"]  = profile.audioInputProfile.encoderToken;
    audioInput["acodec"]        = profile.audioInputProfile.acodec;
    audioInput["channelNo"]     = profile.audioInputProfile.channelNo;
    audioInput["sampleRate"]    = profile.audioInputProfile.sampleRate;
    audioInput["sampleBit"]     = profile.audioInputProfile.sampleBit;
    ret["audioInputProfile"]    = audioInput;
    Json::Value relayProfiles = Json::arrayValue;
    for (const auto &relay : profile.relayOutputProfiles) {
        Json::Value r = Json::objectValue;
        r["token"]    = relay.token;
        r["name"]     = relay.name;
        r["isActive"] = relay.isActive;
        relayProfiles.append(r);
    }
    ret["relayOutputProfiles"] = relayProfiles;
    return ret;
}

static Json::Value makeDeviceCapabilitiesJson(const DeviceCapabilities &caps) {
    Json::Value ret = Json::objectValue;
    ret["isOnvifDevice"] = caps.isOnvifDevice;
    ret["onvifProfile"] = makeOnvifProfileJson(caps.onvifProfile);
    return ret;
}

static Json::Value makeVideoEncoderConfigMapJson(const VideoEncoderConfig::VideoEncoderConfigMap &config_map) {
    Json::Value ret = Json::objectValue;
    for (const auto &it : config_map) {
        const auto &profile_token = it.first;
        const auto &config = it.second;
        Json::Value config_json = Json::objectValue;
        config_json["vcodec"] = config.vcodec;
        config_json["width"] = config.width;
        config_json["height"] = config.height;
        config_json["bitrate"] = config.bitrate;
        config_json["fps"] = config.fps;
        config_json["quality"] = config.quality;
        config_json["state"]["retry_time"] = config.state.retry_time;
        config_json["state"]["status"] = config.state.status;
        ret[profile_token] = config_json;
    }
    return ret;
}

static Json::Value makeDeviceStatisticJson(const DeviceStatistic &stats) {
    Json::Value ret = Json::objectValue;
    ret["connect"] = stats.connect;
    ret["status"] = stats.status;
    ret["capabilities"] = makeDeviceCapabilitiesJson(stats.device_caps);
    ret["user_presets"] = makeOnvifPTZPresetMapJson(stats.user_presets);
    ret["stream_settings"] = makeVideoEncoderConfigMapJson(stats.stream_settings);
    return ret;
}

static OnvifPTZProfile::PTZPresetMap getOnvifPTZPresetMap(const Json::Value &data) {
    OnvifPTZProfile::PTZPresetMap ret;
    if (!data.isNull() && data.isArray()) {
        for (const auto &preset : data) {
            OnvifPTZProfile::PTZPreset p;
            p.Token = preset["Token"].asString();
            p.Name = preset["Name"].asString();
            p.absPan = preset["Pan"].asFloat();
            p.absTilt = preset["Tilt"].asFloat();
            p.absZoom = preset["Zoom"].asFloat();
            ret.emplace(p.Token, std::move(p));
        }
    }
    return ret;
}

static OnvifProfile getOnvifProfile(const Json::Value &data) {
    OnvifProfile profile;
    // mediaProfiles
    for (const auto &mp_json : data["mediaProfiles"]) {
        OnvifMediaProfile mp;
        mp.token = mp_json["token"].asString();
        mp.url = mp_json["url"].asString();
        mp.vcodec = mp_json["vcodec"].asString();
        mp.width = mp_json["width"].asInt();
        mp.height = mp_json["height"].asInt();
        mp.bitrate = mp_json["bitrate"].asInt();
        mp.fps = mp_json["fps"].asFloat();
        for (const auto &vo_json : mp_json["videoEncoder"]["available"].getMemberNames()) {
            VideoEncoderConfigOption vo;
            vo.FrameRatesSupported = mp_json["videoEncoder"]["available"][vo_json]["fps"]["supported"].asString();
            vo.FPSEditable = mp_json["videoEncoder"]["available"][vo_json]["fps"]["editable"].asBool();
            vo.BitRateRange = mp_json["videoEncoder"]["available"][vo_json]["bitrate"]["supported"].asString();
            vo.bitrateEditable = mp_json["videoEncoder"]["available"][vo_json]["bitrate"]["editable"].asBool();
            for (const auto &r_json : mp_json["videoEncoder"]["available"][vo_json]["ResolutionsAvailable"]["supported"]) {
                int width = r_json["width"].asInt();
                int height = r_json["height"].asInt();
                vo.ResolutionsAvailable.emplace_back(width, height);
            }
            vo.resolutionEditable = mp_json["videoEncoder"]["available"][vo_json]["ResolutionsAvailable"]["editable"].asBool();
            mp.vEncoderOptionMap.emplace(vo_json, std::move(vo));
        }
        mp.videoEncEditable = mp_json["videoEncEditable"].asBool();
        mp.videoConfigEditable = mp_json["videoConfigEditable"].asBool();
        profile.mediaProfiles.push_back(mp);
    }
    // ptzProfile
    profile.ptzProfile.strMediaProfileToken = data["ptzProfile"]["strMediaProfileToken"].asString();
    profile.ptzProfile.isAbsMoveEnable = data["ptzProfile"]["isAbsMoveEnable"].asBool();
    profile.ptzProfile.isConsMoveEnable = data["ptzProfile"]["isConsMoveEnable"].asBool();
    profile.ptzProfile.isRelMoveEnable = data["ptzProfile"]["isRelMoveEnable"].asBool();
    profile.ptzProfile.isPresetEnable = data["ptzProfile"]["isPresetEnable"].asBool();
    profile.ptzProfile.presetMap = getOnvifPTZPresetMap(data["ptzProfile"]["presetMap"]);
    profile.ptzProfile.isHomePresetEnable = data["ptzProfile"]["isHomePresetEnable"].asBool();
    profile.ptzProfile.homePresetToken = data["ptzProfile"]["homePresetToken"].asString();
    // deviceInfo
    profile.deviceInfo.manufacturer = data["deviceInfo"]["manufacturer"].asString();
    profile.deviceInfo.model = data["deviceInfo"]["model"].asString();
    profile.deviceInfo.firmwareVersion = data["deviceInfo"]["firmwareVersion"].asString();
    profile.deviceInfo.serialNumber = data["deviceInfo"]["serialNumber"].asString();
    profile.deviceInfo.hardwareId = data["deviceInfo"]["hardwareId"].asString();
    profile.deviceInfo.macAddress = data["deviceInfo"]["macAddress"].asString();
    // image profile
    if (!data["imageProfile"].isNull() && data["imageProfile"].isObject()) {
        profile.imageProfile.isFocusEnable       = data["imageProfile"]["isFocusEnable"].asBool();
        profile.imageProfile.isFocusAbsEnable    = data["imageProfile"]["isFocusAbsEnable"].asBool();
        profile.imageProfile.isFocusConsEnable   = data["imageProfile"]["isFocusConsEnable"].asBool();
        profile.imageProfile.isFocusRelEnable    = data["imageProfile"]["isFocusRelEnable"].asBool();
        profile.imageProfile.isFocusAutoSupported= data["imageProfile"]["isFocusAutoSupported"].asBool();
        profile.imageProfile.isIrisEnable        = data["imageProfile"]["isIrisEnable"].asBool();
        profile.imageProfile.isIrisAutoSupported = data["imageProfile"]["isIrisAutoSupported"].asBool();
        profile.imageProfile.irisMin             = data["imageProfile"]["irisMin"].asFloat();
        profile.imageProfile.irisMax             = data["imageProfile"]["irisMax"].asFloat();
    }
    // audio output profile
    if (!data["audioOutputProfile"].isNull() && data["audioOutputProfile"].isObject()) {
        profile.audioOutputProfile.enable       = data["audioOutputProfile"]["enable"].asBool();
        profile.audioOutputProfile.token        = data["audioOutputProfile"]["token"].asString();
        profile.audioOutputProfile.name         = data["audioOutputProfile"]["name"].asString();
        profile.audioOutputProfile.outputToken  = data["audioOutputProfile"]["outputToken"].asString();
        profile.audioOutputProfile.profileToken = data["audioOutputProfile"]["profileToken"].asString();
        profile.audioOutputProfile.outputLevel  = data["audioOutputProfile"]["outputLevel"].asInt();
        profile.audioOutputProfile.sendPrimacy  = data["audioOutputProfile"]["sendPrimacy"].asString();
        profile.audioOutputProfile.decoderToken = data["audioOutputProfile"]["decoderToken"].asString();
        for (const auto &it : data["audioOutputProfile"]["supportedDecodingEncodings"]) {
            profile.audioOutputProfile.supportedDecodingEncodings.push_back(it.asString());
        }
    }
    // audio input profile
    if (!data["audioInputProfile"].isNull() && data["audioInputProfile"].isObject()) {
        profile.audioInputProfile.enable        = data["audioInputProfile"]["enable"].asBool();
        profile.audioInputProfile.token         = data["audioInputProfile"]["token"].asString();
        profile.audioInputProfile.name          = data["audioInputProfile"]["name"].asString();
        profile.audioInputProfile.sourceToken   = data["audioInputProfile"]["sourceToken"].asString();
        profile.audioInputProfile.encoderToken  = data["audioInputProfile"]["encoderToken"].asString();
        profile.audioInputProfile.acodec        = data["audioInputProfile"]["acodec"].asString();
        profile.audioInputProfile.channelNo     = data["audioInputProfile"]["channelNo"].asInt();
        profile.audioInputProfile.sampleRate    = data["audioInputProfile"]["sampleRate"].asString();
        profile.audioInputProfile.sampleBit     = data["audioInputProfile"]["sampleBit"].asString();
    }
    // relay output profiles
    if (!data["relayOutputProfiles"].isNull() && data["relayOutputProfiles"].isArray()) {
        for (const auto &r : data["relayOutputProfiles"]) {
            OnvifRelayOutputProfile relay;
            relay.token    = r["token"].asString();
            relay.name     = r["name"].asString();
            relay.isActive = r["isActive"].asBool();
            profile.relayOutputProfiles.push_back(relay);
        }
    }
    return profile;
}

static DeviceCapabilities getDeviceCapabilities(const Json::Value &data) {
    DeviceCapabilities stats;
    stats.isOnvifDevice = !data["isOnvifDevice"].empty() ? data["isOnvifDevice"].asBool() : false;
    stats.onvifProfile = getOnvifProfile(data["onvifProfile"]);
    return stats;
}

static VideoEncoderConfig::VideoEncoderConfigMap getVideoEncoderConfigMap(const Json::Value &data) {
    VideoEncoderConfig::VideoEncoderConfigMap ret;
    for (const auto &it : data.getMemberNames()) {
        const auto &profile_token = it;
        const auto &config_json = data[it];
        VideoEncoderConfig config;
        config.vcodec = config_json["vcodec"].asString();
        config.width = config_json["width"].asInt();
        config.height = config_json["height"].asInt();
        config.bitrate = config_json["bitrate"].asInt();
        config.fps = config_json["fps"].asFloat();
        config.quality = config_json["quality"].asFloat();
        config.state.retry_time = config_json["state"]["retry_time"].asInt();
        config.state.status = config_json["state"]["status"].asString();
        ret.emplace(profile_token, std::move(config));
    }
    return ret;
}

static DeviceStatistic getDeviceStatistic(const Json::Value &data) {
    DeviceStatistic stats;
    stats.connect = !data["connect"].empty() ? data["connect"].asBool() : false;
    stats.status = !data["status"].empty() ? data["status"].asString() : "";
    if (!data["capabilities"].empty()) {
        stats.device_caps = getDeviceCapabilities(data["capabilities"]);
    }
    if (!data["user_presets"].empty()) {
        stats.user_presets = getOnvifPTZPresetMap(data["user_presets"]);
    }
    if (!data["stream_settings"].empty()) {
        stats.stream_settings = getVideoEncoderConfigMap(data["stream_settings"]);
    }
    return stats;
}

// StreamTuple
static Json::Value makeStreamTupleJson(unordered_map<int, StreamTuple> stream_map, int stream_type) {
    Json::Value ret = Json::objectValue;
    if (stream_map.find(stream_type) != stream_map.end()) {
        ret["id"] = stream_map[stream_type].stream_id;
        ret["url"] = stream_map[stream_type].full_url;
    }
    return ret;
}

static StreamTuple getStreamTuple(const Json::Value &data, const DeviceTuple &tuple, int stream_type) {
    StreamTuple stream;
    stream.vhost = tuple.vhost;
    stream.device_id = tuple.device_id;
    stream.name = getStreamTypeString(stream_type);
    stream.stream_id = data["id"].asString();
    stream.full_url = data["url"].asString();
    return stream;
}

// MotionStorageStats
static Json::Value makeMotionStorageStatsJson(const MotionStorageStats &stats) {
    Json::Value ret = Json::objectValue;
    ret["archiveStartTime"] = stats.archiveStartTime;
    ret["archiveEndTime"] = stats.archiveEndTime;
    return ret;
}

static MotionStorageStats getMotionStorageStats(const Json::Value &data) {
    MotionStorageStats stats;
    stats.archiveStartTime = !data["archiveStartTime"].empty() ? data["archiveStartTime"].asUInt64() : 0;
    stats.archiveEndTime = !data["archiveEndTime"].isNull() ? data["archiveEndTime"].asUInt64() : 0;
    return stats;
}

// TierStorageStats
static Json::Value makeTierStorageStatsJson(const unordered_map<int, TierStorageStats> stats_map, int tier_type) {
    Json::Value ret = Json::objectValue;
    auto it_statistic = stats_map.find(tier_type);
    if (it_statistic != stats_map.end()) {
        ret["archiveStartTime"] = it_statistic->second.archiveStartTime;
        ret["archiveEndTime"] = it_statistic->second.archiveEndTime;
    }
    return ret;
}

static TierStorageStats getTierStorageStats(const Json::Value &data) {
    TierStorageStats stats;
    stats.archiveStartTime = !data["archiveStartTime"].empty() ? data["archiveStartTime"].asUInt64() : 0;
    stats.archiveEndTime = !data["archiveEndTime"].empty() ? data["archiveEndTime"].asUInt64() : 0;
    return stats;
}

// ################### CameraStatisticHelper ###########################

bool CameraStatisticHelper::getParams(const string &json_str, CameraStatistic &stats) {
    Json::Value ret;
    if (!StrJsonUtils::readJsonString(json_str, ret)) {
        WarnL << "Parse json string failed";
        return false;
    }
    DeviceTuple tuple;
    tuple.vhost = ret["vhost"].asString();
    tuple.device_id = ret["id"].asString();
    tuple.name = ret["name"].asString();
    stats.tuple = tuple;

    CameraOption option;
    option.name = ret["name"].asString();
    option.ip = ret["ip"].asString();
    option.port = ret["port"].asInt();
    option.manufacturer = ret["manufacturer"].asString();
    option.model = ret["model"].asString();
    option.username = ret["username"].asString();
    option.password = ret["password"].asString();
    option.disablePrimaryStream = ret["disablePrimaryStream"].asBool();
    option.disableSecondaryStream = ret["disableSecondaryStream"].asBool();
    option.doNotRecordPrimaryStream = ret["doNotRecordPrimaryStream"].asBool();
    option.doNotRecordSecondaryStream = ret["doNotRecordSecondaryStream"].asBool();
    option.enableActive = ret["enableActive"].asBool();
    option.enableRecord = ret["enableRecord"].asBool();
    option.recordRootPath = ret["recordRootPath"].asString();
    option.keepArchivedMaxFor = ret["keepArchivedMaxFor"].asUInt64();
    option.keepArchivedMaxForAuto = ret["keepArchivedMaxForAuto"].asBool();
    option.keepArchivedMinFor = ret["keepArchivedMinFor"].asUInt64();
    option.keepArchivedMinForAuto = ret["keepArchivedMinForAuto"].asBool();
    option.rtpTransport = ret["rtpTransport"].asInt();
    option.autoMediaPort = ret["autoMediaPort"].asBool();
    option.mediaPort = ret["mediaPort"].asInt();
    option.enableFailover = ret["enableFailover"].asBool();
    option.preferedMediaServer = ret["preferedMediaServer"].asString();
    option.enablePTZControl = ret["enablePTZControl"].asBool();
    // Absent in statistics written before the option existed: keep its default (on).
    option.enableTimeSync = ret.isMember("enableTimeSync") ? ret["enableTimeSync"].asBool() : true;
    option.motionPreRecordSec = ret["motionPreRecordSec"].asInt();
    option.motionPostRecordSec = ret["motionPostRecordSec"].asInt();
    option.recordSchedules = ret["recordSchedules"].asString();
    option.disableAudio = ret["disableAudio"].asBool();
    option.webPort = ret["webPort"].asInt();
    option.autoWebPort = ret["autoWebPort"].asBool();
    option.keepConfigProfileAndStream = ret["keepConfigProfileAndStream"].asBool();
    option.reversePanAxis = ret["reversePanAxis"].asBool();
    option.reverseTiltAxis = ret["reverseTiltAxis"].asBool();
    option.ptzMode = ret["ptzMode"].asInt();
    option.ptzSpeed = ret["ptzSpeed"].asFloat();
    option.onvifMainProfile = ret["onvifMainProfile"].asString();
    option.onvifSubProfile = ret["onvifSubProfile"].asString();
    option.enableMotion = ret["enableMotion"].asBool();
    option.roiValue = ret["roiValue"].asString();
    option.motionDetectOnStream = ret["motionDetectOnStream"].asInt();
    option.sdCardSyncEnabled = ret["sdCardSyncEnabled"].asBool();
    option.sdCardSyncAutoSyncEnabled = ret["sdCardSyncAutoSyncEnabled"].asBool();
    option.sdCardSyncMinSegmentGapSec = ret["sdCardSyncMinSegmentGapSec"].asInt();
    option.sdCardSyncRetryCount = ret["sdCardSyncRetryCount"].asInt();
    option.enforceWatermarkOnView = ret["enforceWatermarkOnView"].asBool();
    option.enableReduceQualityOnView = ret["enableReduceQualityOnView"].asBool();
    option.watermarkExcludedRoleIds = ret["watermarkExcludedRoleIds"].asString();
    option.watermarkTemplate = ret["watermarkTemplate"].asString();
    option.enforcePrivacyMaskOnView = ret["enforcePrivacyMaskOnView"].asBool();
    option.privacyMaskExcludedRoleIds = ret["privacyMaskExcludedRoleIds"].asString();
    option.privacyMaskRegions = ret["privacyMaskRegions"].asString();
    stats.option = option;

    // stream tuple map
    unordered_map<int, StreamTuple> stream_map;
    auto streamUrlsString = ret["streamUrls"].asString();
    Json::Value streamUrlsJson;
    StrJsonUtils::readJsonString(streamUrlsString, streamUrlsJson);
    auto primary_stream = getStreamTuple(streamUrlsJson[PrimaryStream], tuple, PrimaryStream);
    if (!primary_stream.empty()) {
        stream_map[PrimaryStream] = primary_stream;
    }
    auto secondary_stream = getStreamTuple(streamUrlsJson[SecondaryStream], tuple, SecondaryStream);
    if (!secondary_stream.empty()) {
        stream_map[SecondaryStream] = secondary_stream;
    }
    stats.stream_map = stream_map;

    // add params
    for (const auto &it : ret["addParams"]) {
        if (it["name"] == "bookmarkStats") {
            Json::Value bm_json;
            StrJsonUtils::readJsonString(it["value"].asString(), bm_json);
            stats.bm = getBookmarkStats(bm_json);
        } else if (it["name"] == "streamStorageInfos") {
            Json::Value storage_json;
            StrJsonUtils::readJsonString(it["value"].asString(), storage_json);
            if (!primary_stream.empty()) {
                auto stream_id = primary_stream.stream_id;
                stats.storage_map[stream_id] = getStreamStorageStats(storage_json[PrimaryStream]);
            }
            if (!secondary_stream.empty()) {
                auto stream_id = secondary_stream.stream_id;
                stats.storage_map[stream_id] = getStreamStorageStats(storage_json[SecondaryStream]);
            }
        } else if (it["name"] == "streamStatisticInfos") {
            Json::Value stream_stats_json;
            StrJsonUtils::readJsonString(it["value"].asString(), stream_stats_json);
            if (!primary_stream.empty()) {
                stats.sinfo_map[PrimaryStream] = getStreamStatistic(stream_stats_json[PrimaryStream]);
            }
            if (!secondary_stream.empty()) {
                stats.sinfo_map[SecondaryStream] = getStreamStatistic(stream_stats_json[SecondaryStream]);
            }
        } else if (it["name"] == "deviceStatistic") {
            Json::Value device_stats_json;
            StrJsonUtils::readJsonString(it["value"].asString(), device_stats_json);
            stats.device_stats = getDeviceStatistic(device_stats_json);
        } else if (it["name"] == "motionStorageStats") {
            Json::Value motion_storage_json;
            StrJsonUtils::readJsonString(it["value"].asString(), motion_storage_json);
            stats.motion_stats = getMotionStorageStats(motion_storage_json);
        } else if (it["name"] == "transportStats") {
            Json::Value transport_stats_json;
            StrJsonUtils::readJsonString(it["value"].asString(), transport_stats_json);
            stats.transport_stats = getTransportStats(transport_stats_json);
        } else if (it["name"] == "tierStorageStats") {
#ifdef ENABLE_TIER_STORAGE
            Json::Value tier_storage_json;
            StrJsonUtils::readJsonString(it["value"].asString(), tier_storage_json);
            stats.tier_storage_map[HotTier] = getTierStorageStats(tier_storage_json[HotTier]);
            stats.tier_storage_map[WarmTier] = getTierStorageStats(tier_storage_json[WarmTier]);
            stats.tier_storage_map[ColdTier] = getTierStorageStats(tier_storage_json[ColdTier]);
#endif // ENABLE_TIER_STORAGE 
        }
    }

    // created_at/updated_at
    stats.created_at = ret["created_at"].asUInt64();
    stats.updated_at= ret["updated_at"].asUInt64();

    return true;
}

string CameraStatisticHelper::getParamsString(const CameraStatistic &stats) {
    Json::Value root;
    // camera info
    root["vhost"] = stats.tuple.vhost;
    root["id"] = stats.tuple.device_id;
    
    // camera option
    root["name"] = stats.option.name;
    root["ip"] = stats.option.ip;
    root["port"] = stats.option.port;
    root["manufacturer"] = stats.option.manufacturer;
    root["model"] = stats.option.model;
    root["username"] = stats.option.username;
    root["password"] = stats.option.password;
    root["disablePrimaryStream"] = stats.option.disablePrimaryStream;
    root["disableSecondaryStream"] = stats.option.disableSecondaryStream;
    root["doNotRecordPrimaryStream"] = stats.option.doNotRecordPrimaryStream;
    root["doNotRecordSecondaryStream"] = stats.option.doNotRecordSecondaryStream;
    root["enableActive"] = stats.option.enableActive;
    root["enableRecord"] = stats.option.enableRecord;
    root["recordRootPath"] = stats.option.recordRootPath;
    root["keepArchivedMaxFor"] = stats.option.keepArchivedMaxFor;
    root["keepArchivedMaxForAuto"] = stats.option.keepArchivedMaxForAuto;
    root["keepArchivedMinFor"] = stats.option.keepArchivedMinFor;
    root["keepArchivedMinForAuto"] = stats.option.keepArchivedMinForAuto;
    root["rtpTransport"] = stats.option.rtpTransport;
    root["autoMediaPort"] = stats.option.autoMediaPort;
    root["mediaPort"] = stats.option.mediaPort;
    root["enableFailover"] = stats.option.enableFailover;
    root["preferedMediaServer"] = stats.option.preferedMediaServer;
    root["enablePTZControl"] = stats.option.enablePTZControl;
    root["enableTimeSync"] = stats.option.enableTimeSync;
    root["motionPreRecordSec"] = stats.option.motionPreRecordSec;
    root["motionPostRecordSec"] = stats.option.motionPostRecordSec;
    root["recordSchedules"] = stats.option.recordSchedules;
    root["disableAudio"] = stats.option.disableAudio;
    root["webPort"] = stats.option.webPort;
    root["autoWebPort"] = stats.option.autoWebPort;
    root["keepConfigProfileAndStream"] = stats.option.keepConfigProfileAndStream;
    root["reversePanAxis"] = stats.option.reversePanAxis;
    root["reverseTiltAxis"] = stats.option.reverseTiltAxis;
    root["ptzMode"] = stats.option.ptzMode;
    root["ptzSpeed"] = stats.option.ptzSpeed;
    root["onvifMainProfile"] = stats.option.onvifMainProfile;
    root["onvifSubProfile"] = stats.option.onvifSubProfile;
    root["enableMotion"] = stats.option.enableMotion;
    root["roiValue"] = stats.option.roiValue;
    root["motionDetectOnStream"] = stats.option.motionDetectOnStream;
    root["sdCardSyncEnabled"] = stats.option.sdCardSyncEnabled;
    root["sdCardSyncAutoSyncEnabled"] = stats.option.sdCardSyncAutoSyncEnabled;
    root["sdCardSyncMinSegmentGapSec"] = stats.option.sdCardSyncMinSegmentGapSec;
    root["sdCardSyncRetryCount"] = stats.option.sdCardSyncRetryCount;
    root["enforceWatermarkOnView"] = stats.option.enforceWatermarkOnView;
    root["enableReduceQualityOnView"] = stats.option.enableReduceQualityOnView;
    root["watermarkExcludedRoleIds"] = stats.option.watermarkExcludedRoleIds;
    root["watermarkTemplate"] = stats.option.watermarkTemplate;
    root["enforcePrivacyMaskOnView"] = stats.option.enforcePrivacyMaskOnView;
    root["privacyMaskExcludedRoleIds"] = stats.option.privacyMaskExcludedRoleIds;
    root["privacyMaskRegions"] = stats.option.privacyMaskRegions;

    // stream tuple map
    Json::Value streamUrls = Json::arrayValue;
    streamUrls[PrimaryStream] = makeStreamTupleJson(stats.stream_map, PrimaryStream);
    streamUrls[SecondaryStream] = makeStreamTupleJson(stats.stream_map, SecondaryStream);
    root["streamUrls"] = StrJsonUtils::writeJsonString(streamUrls);
    // add params
    Json::Value params = Json::arrayValue;
    Json::Value bm_json = makeBookmarkStatsJson(stats.bm);
    params.append(makeJsonKeyValue("bookmarkStats", StrJsonUtils::writeJsonString(bm_json)));
    Json::Value storage_json = Json::arrayValue;
    storage_json.append(makeStreamStorageStatsJson(stats.storage_map, stats.stream_map, PrimaryStream));
    storage_json.append(makeStreamStorageStatsJson(stats.storage_map, stats.stream_map, SecondaryStream));
    params.append(makeJsonKeyValue("streamStorageInfos", StrJsonUtils::writeJsonString(storage_json)));

    Json::Value stream_stats_json = Json::arrayValue;
    stream_stats_json.append(makeStreamStatisticJson(stats.sinfo_map, PrimaryStream));
    stream_stats_json.append(makeStreamStatisticJson(stats.sinfo_map, SecondaryStream));
    params.append(makeJsonKeyValue("streamStatisticInfos", StrJsonUtils::writeJsonString(stream_stats_json)));

    Json::Value device_stats_json = makeDeviceStatisticJson(stats.device_stats);
    params.append(makeJsonKeyValue("deviceStatistic", StrJsonUtils::writeJsonString(device_stats_json)));

    Json::Value motion_storage_json = makeMotionStorageStatsJson(stats.motion_stats);
    params.append(makeJsonKeyValue("motionStorageStats", StrJsonUtils::writeJsonString(motion_storage_json)));

    Json::Value transport_stats_json = makeTransportStatsJson(stats.transport_stats);
    params.append(makeJsonKeyValue("transportStats", StrJsonUtils::writeJsonString(transport_stats_json)));

#ifdef ENABLE_TIER_STORAGE
    Json::Value tier_storage_json = Json::arrayValue;
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, HotTier));
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, WarmTier));
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, ColdTier));
    params.append(makeJsonKeyValue("tierStorageStats", StrJsonUtils::writeJsonString(tier_storage_json)));
#endif
    root["addParams"] = params;

    // created_at/updated_at
    root["created_at"] = stats.created_at;
    root["updated_at"] = stats.updated_at;

    return root.toStyledString();
}

// ################### CameraStatisticImp ###########################

CameraStatisticImp::CameraStatisticImp(const std::string &src_path, int sync_interval_sec) : _sync_interval_sec(sync_interval_sec) {
    CHECK(!src_path.empty(), "Source path cannot be empty");
    // One serial queue per camera keeps its ESC writes (assign/release, upsert, status, remove) in
    // order; different cameras run in parallel. A pool, not a poller: a locked DB waits out busy_timeout.
    _esc_queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::api(), "esc " + src_path);
    setup(src_path);
}

CameraStatisticImp::~CameraStatisticImp() {}

void CameraStatisticImp::setup(const string &src_path) {
    auto file_path = src_path;
    if (!end_with(file_path, "/info.txt")) {
        file_path += "/info.txt";
    }
    _file = std::make_shared<FileRecorder<CameraStatistic, CameraStatisticHelper>>(file_path);
    if (!_file->empty()) {
        load();
    }
}

void CameraStatisticImp::load() {
    CameraStatistic saved_stats;
    if (_file->load(saved_stats)) {
        // Manually assign fields from stats to this, include camera_info and stream_map
        tuple = saved_stats.tuple;
        stream_map = saved_stats.stream_map;
        option = saved_stats.option;
        bm = saved_stats.bm;
        storage_map = saved_stats.storage_map;
        sinfo_map = saved_stats.sinfo_map;
        transport_stats = saved_stats.transport_stats;
        device_stats = saved_stats.device_stats;
        motion_stats = saved_stats.motion_stats;
        tier_storage_map = saved_stats.tier_storage_map;
        created_at = saved_stats.created_at;
        updated_at = saved_stats.updated_at;
    }
}

void CameraStatisticImp::save() {
    if (created_at == 0) {
        created_at = time(nullptr);
    }
    updated_at = time(nullptr);
    _file->save(static_cast<const CameraStatistic &>(*this));
    // Statistic-only saves do not push ESC on every call. Two exceptions keep ESC consistent:
    //  - retry a config sync that was not written yet (sync mode was off or the write failed);
    //  - a periodic refresh every _sync_interval_sec. It is a no-op when ESC already matches, and
    //    repairs values overwritten by a stale peer write (UPSERT_BATCH has no per-item LWW yet)
    //    or a resource removed by another node.
    if (!_esc_dirty && _sync_mode && static_cast<uint64_t>(time(nullptr)) - _last_sync_time >= static_cast<uint64_t>(_sync_interval_sec)) {
        _esc_dirty = true;
    }
    if (_esc_dirty) {
        syncToEsc(false);
    }
}

void CameraStatisticImp::markEscConfigChanged() {
    _esc_dirty = true;
    syncToEsc(true);
}

void CameraStatisticImp::setDeviceTuple(const DeviceTuple &input_tuple) {
    std::lock_guard<std::mutex> lck(_mtx);
    tuple = input_tuple;
    save();
    markEscConfigChanged();
}

void CameraStatisticImp::setStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto it_primary = input_stream_map.find(PrimaryStream);
    if (it_primary != input_stream_map.end()) {
        auto stream = it_primary->second;
        if (!stream.empty()) {
            // Ensure primary stream exist in storage_map and sinfo_map
            if (storage_map.find(stream.stream_id) == storage_map.end()) {
                storage_map[stream.stream_id] = StreamStorageStats();
            }
            if (sinfo_map.find(PrimaryStream) == sinfo_map.end()) {
                sinfo_map[PrimaryStream] = StreamStatistic();
            }
        } else {
            WarnL << "Input primary stream is empty";
        }
    } else {
        WarnL << "Input stream map do not have primary stream";
    }

    auto it_secondary = input_stream_map.find(SecondaryStream);
    if (it_secondary != input_stream_map.end()) {
        auto stream = it_secondary->second;
        if (!stream.empty()) {
            // Ensure secondary stream exist in storage_map and sinfo_map
            if (storage_map.find(stream.stream_id) == storage_map.end()) {
                storage_map[stream.stream_id] = StreamStorageStats();
            }
            if (sinfo_map.find(SecondaryStream) == sinfo_map.end()) {
                sinfo_map[SecondaryStream] = StreamStatistic();
            }
        } else {
            WarnL << "Input secondary stream is empty";
        }
    } else {
        WarnL << "Input stream map do not have secondary stream";
    }
    
    stream_map = input_stream_map;
    save();
    markEscConfigChanged();
}

void CameraStatisticImp::setCameraOption(const CameraOption &input_option) {
    std::lock_guard<std::mutex> lck(_mtx);
    option = input_option;

    if (option.enableActive) {
        // Active camera need to assign resource
        assignResource(true);
    } else if (option.enableFailover) {
        // Camera is inactive but enable failover, also need to release resource
        assignResource(false);
    }
    save();
    markEscConfigChanged();
}

void CameraStatisticImp::setTransportStats(const TransportStats &input_stats) {
    std::lock_guard<std::mutex> lck(_mtx);
    transport_stats = input_stats;
    save();
}

std::string CameraStatisticImp::getStreamID(const int &stream_type) {
    return stream_map[stream_type].stream_id;
}

std::unordered_map<std::string, VideoEncoderConfig> CameraStatisticImp::getStreamVideoEncoderConfigMap() {
    const auto& profiles = device_stats.device_caps.onvifProfile.mediaProfiles;
    std::unordered_map<std::string, VideoEncoderConfig> result;

    std::unordered_map<std::string, const OnvifMediaProfile*> profileByUrl;
    for (const auto& profile : profiles) {
        profileByUrl[profile.url] = &profile;
    }

    for (const auto& streamPair : stream_map) {
        const StreamTuple& stream = streamPair.second;
        auto it = profileByUrl.find(stream.full_url);
        if (it == profileByUrl.end()) continue;

        const OnvifMediaProfile* profile = it->second;
        VideoEncoderConfig vConfig;
        vConfig.vcodec  = profile->vcodec;
        vConfig.width   = profile->width;
        vConfig.height  = profile->height;
        vConfig.bitrate = profile->bitrate;
        vConfig.fps     = profile->fps;
        result[stream.stream_id] = vConfig;
    }

    return result;
}

void CameraStatisticImp::addArchiveSize(string stream_id, size_t count, size_t size, uint64_t archived_start_time, uint64_t archived_end_time, bool add) {
    std::lock_guard<std::mutex> lck(_mtx);
    // Do not process time blocks with an end time earlier than the info file creation time, so that old media data does not need to be re-aggregated
    if (archived_end_time <= created_at) {
        DebugL << "Time block has end time (" << archived_end_time << ") less than or equal created_at of file ("<< created_at <<"). Ignore" ;
        return;
    }

    if (storage_map.find(stream_id) != storage_map.end()) {
        auto &storage = storage_map[stream_id]; 
        if (add) {
            storage.archiveSizeB += size;
            storage.archiveIndexRecordCount += count;
            if (storage.archiveStartTime == 0) {
                storage.archiveStartTime = archived_start_time;
            }
            storage.archiveEndTime = archived_end_time;
            DebugL << "Stream " << tuple.shortUrl() << "/" << stream_id << " add archived size: " << format_bytes_human_readable(size) << ", count: " << count
                   << ". Total archived size: " << format_bytes_human_readable(storage.archiveSizeB)
                   << ". Total archived count: " << storage.archiveIndexRecordCount
                   << ". First archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveStartTime)
                   << ". Last archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveEndTime);
        } else {
            storage.archiveSizeB = storage.archiveSizeB >= size ? (storage.archiveSizeB - size) : 0;
            storage.archiveIndexRecordCount = storage.archiveIndexRecordCount >= count ? (storage.archiveIndexRecordCount - count) : 0;
            if (storage.archiveIndexRecordCount == 0) {
                // record count is equal 0, reset archiveStartTime and archiveEndTime equal 0 to indicate no data
                storage.archiveStartTime = 0;
                storage.archiveEndTime = 0;
            } else {
                // Note: Use the end time block for approximate statistics, not completely accurate. Use the TimeQuery::getFirstBlock function to get the exact number.
                storage.archiveStartTime = archived_end_time;
            }
            DebugL << "Stream " << tuple.shortUrl() << "/" << stream_id << " subtract archived size: " << format_bytes_human_readable(size) << ", count: " << count
                   << ". Total archived size: " << format_bytes_human_readable(storage.archiveSizeB)
                   << ". Total archived count: " << storage.archiveIndexRecordCount
                   << ". First archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveStartTime)
                   << ". Last archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveEndTime);
        }
        save();
    } else {
        WarnL << "Camera " << tuple.shortUrl() << " do not have stream: " << stream_id << ". Ignore archive size statistic";
    }
}

void CameraStatisticImp::addBookmarkCount(uint64_t bm_created_at, size_t size, bool add) {
    std::lock_guard<std::mutex> lck(_mtx);
    // Do not process bookmark with an creation time earlier than the info file creation time, so that old media data does not need to be re-aggregated
    if (bm_created_at <= created_at) {
        DebugL << "Bookmark has creation time (" << bm_created_at << ") less than or equal created_at of file ("<< created_at <<"). Ignore" ;
        return;
    }
    if (add) {
        bm.recordCount ++;
        bm.recordAverageSizeB += size;
        DebugL << "Camera " << tuple.shortUrl() << " add bookmark count: 1. Total bookmark count: " << bm.recordCount;

    } else {
        if (bm.recordCount > 0) {
            bm.recordCount--;
        }
        if (bm.recordAverageSizeB >= size) {
            bm.recordAverageSizeB -= size;
        } else {
            bm.recordAverageSizeB = 0;
        }
        DebugL << "Camera " << tuple.shortUrl() << " subtract bookmark count: 1. Total bookmark count: " << bm.recordCount;
    }
    save();
}

CameraStatistic CameraStatisticImp::getParams() {
    std::lock_guard<std::mutex> lck(_mtx);
    return static_cast<const CameraStatistic &>(*this);
}

void CameraStatisticImp::addStreamStatistic(int stream_type, bool live, string status, const TranslationInfo *info_) {
    bool save_status_history = false;
    std::string stream_id;
    std::string stream_url;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        if (sinfo_map.find(stream_type) == sinfo_map.end()) {
            WarnL << "Camera " << tuple.shortUrl() << " do not have stream type: " << stream_type << ". Ignore add stream statistic";
            return;
        }
        auto &sinfo = sinfo_map[stream_type];
        save_status_history = _status_history_initialized_streams.emplace(stream_type).second ||
                              sinfo.live != live || (!live && sinfo.status != status);
        if (!sinfo.last_change_status || sinfo.live != live) {
            sinfo.last_change_status = time(nullptr);
        }
        sinfo.live = live;
        sinfo.status = status;
        sinfo.byte_speed = live && info_ ? info_->byte_speed : 0;
        if (live && info_) {
            for (const auto &it : info_->stream_info) {
                switch(it.codec_type) {
                    case TrackVideo: 
                        sinfo.has_video = true;
                        sinfo.vcodec = it.codec_name;
                        sinfo.source_vcodec = it.origin_codec_name;
                        sinfo.transcoded = it.origin_codec_name != it.codec_name;
                        sinfo.width = it.video_width;
                        sinfo.height = it.video_height;
                        sinfo.bitrate = it.bitrate;
                        sinfo.fps = it.video_fps;
                        break;
                    case TrackAudio:
                        sinfo.has_audio = true;
                        sinfo.acodec = it.codec_name;
                        sinfo.sample_rate = it.audio_sample_rate;
                        sinfo.channel_no = it.audio_channel;
                        sinfo.sample_bit = it.audio_sample_bit;
                        break;
                    default:
                        break;
                }
            }
        }
        DebugL << "Stream " << stream_map[stream_type].shortUrl() << " statistic: Live=" << sinfo.live << ". Status=" << sinfo.status << ". Byte_speed=" << sinfo.byte_speed << " bytes/s";
        save();
        syncResourceStatus();
        auto stream_it = stream_map.find(stream_type);
        if (stream_it != stream_map.end() && !stream_it->second.empty()) {
            stream_id = stream_it->second.stream_id;
            stream_url = stream_it->second.shortUrl();
        }
    }
    if (save_status_history) {
        saveStreamStatusHistory(stream_id, stream_url, live, status);
    }
}

void CameraStatisticImp::remove(bool failover_active) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        if (_file) {
            _file->remove();
            DebugL << "Removed file recorder success: " << tuple.shortUrl();
            removed = true;
        }
        if (!failover_active) {
            // Queued behind any pending ESC writes of this camera, so they cannot recreate it afterwards.
            removeFromEsc();
        }
        _esc_removed = true;
    }

    if (_on_remove && removed) {
        _on_remove(tuple.device_id);
    }
}

void CameraStatisticImp::addDeviceCapabilities(bool connect, string status, const DeviceCapabilities *device_caps) {
    bool save_status_history = false;
    std::string device_id;
    std::string device_url;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        save_status_history = !_device_status_history_initialized || device_stats.connect != connect ||
                              (!connect && device_stats.status != status);
        _device_status_history_initialized = true;
        device_stats.connect = connect;
        device_stats.status = status;
        if (device_caps) {
            device_stats.device_caps = *device_caps;
        }
        device_id = tuple.device_id;
        device_url = tuple.shortUrl();
        DebugL << "Device " << device_url << " capabilities: Connected=" << device_stats.connect << ", Status=" << device_stats.status
               << ", isOnvifDevice=" << device_stats.device_caps.isOnvifDevice
               << ", onvifProfile.mediaProfiles.size=" << device_stats.device_caps.onvifProfile.mediaProfiles.size();
        save();
    }
    if (save_status_history) {
        saveDeviceStatusHistory(device_id, device_url, connect, status);
    }
}

void CameraStatisticImp::addUserPresets(const std::string &preset_token, const std::string &preset_name, float abs_pan, float abs_tilt, float abs_zoom, bool add) {
    std::lock_guard<std::mutex> lck(_mtx);
    if (!device_stats.device_caps.isOnvifDevice) {
        WarnL << "Camera " << tuple.shortUrl() << " is not an ONVIF device. Ignore add user preset.";
        return;
    }
    auto &ptzProfile = device_stats.device_caps.onvifProfile.ptzProfile;
    if (!ptzProfile.isPresetEnable) {
        WarnL << "Camera " << tuple.shortUrl() << " does not support preset. Ignore add user preset.";
        return;
    }
    if (add) {
        OnvifPTZProfile::PTZPreset preset;
        preset.Token = preset_token;
        preset.Name = preset_name;
        preset.absPan = abs_pan;
        preset.absTilt = abs_tilt;
        preset.absZoom = abs_zoom;
        device_stats.user_presets.emplace(preset.Token, std::move(preset));
        DebugL << "Camera " << tuple.shortUrl() << " add user preset: " << preset_token << ", name: " << preset_name
               << ", absPan: " << abs_pan << ", absTilt: " << abs_tilt << ", absZoom: " << abs_zoom
               << ". Total preset count: " << device_stats.user_presets.size();
    } else {
        auto it_preset = device_stats.user_presets.find(preset_token);
        if (it_preset != device_stats.user_presets.end()) {
            device_stats.user_presets.erase(it_preset);
            DebugL << "Camera " << tuple.shortUrl() << " remove user preset: " << preset_token
                   << ". Total preset count: " << device_stats.user_presets.size();
        } else {
            WarnL << "Camera " << tuple.shortUrl() << " do not have user preset with token: " << preset_token << ". Ignore remove user preset.";
        }
    }
    save();
}

void CameraStatisticImp::addMotionKeepThreshold(bool start, uint64_t threshold) {
    std::lock_guard<std::mutex> lck(_mtx);
    if (start) {
        motion_stats.archiveStartTime = threshold;
        DebugL << "Camera " << tuple.shortUrl() << " set motion keep start threshold: " << threshold << " => " << getTimeStr("%Y-%m-%d %H:%M:%S", threshold);
    } else {
        motion_stats.archiveEndTime = threshold;
        DebugL << "Camera " << tuple.shortUrl() << " set motion keep end threshold: " << threshold << " => " << getTimeStr("%Y-%m-%d %H:%M:%S", threshold);
    }
    save();
}

void CameraStatisticImp::addTierKeepThreshold(int tier_type, bool start, uint64_t threshold) {
#ifdef ENABLE_TIER_STORAGE
    std::lock_guard<std::mutex> lck(_mtx);
    auto &tier_stats = tier_storage_map[tier_type];
    if (start) {
        tier_stats.archiveStartTime = threshold;
        DebugL << "Camera " << tuple.shortUrl() << " set tier " << tierTypeToString(static_cast<TierType>(tier_type)) << " keep start threshold: " << threshold << " seconds";
    } else {
        tier_stats.archiveEndTime = threshold;
        DebugL << "Camera " << tuple.shortUrl() << " set tier " << tierTypeToString(static_cast<TierType>(tier_type)) << " keep end threshold: " << threshold << " seconds";
    }
    save();
#else
    WarnL << "Tier storage is not enabled. Ignore add tier keep threshold.";
#endif
}

void CameraStatisticImp::saveVideoEncoderConfig(const std::string token, const VideoEncoderConfig &config) {
    std::lock_guard<std::mutex> lck(_mtx);
    device_stats.stream_settings[token] = config;
    save();
}

bool CameraStatisticImp::loadVideoEncoderConfig(const std::string token, VideoEncoderConfig &config) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = device_stats.stream_settings.find(token);
    if (it == device_stats.stream_settings.end()) {
        return false;
    }
    config = it->second;
    if (config.vcodec.empty()) {
        return false;
    }
    return true;
}

std::string CameraStatisticImp::getPrimaryProfileToken() const {
    auto stream_it = stream_map.find(PrimaryStream);
    if (stream_it == stream_map.end()) {
        return "";
    }

    const std::string &full_url = stream_it->second.full_url;
    if (full_url.empty()) {
        return "";
    }

    const auto &media_profiles = device_stats.device_caps.onvifProfile.mediaProfiles;
    for (const auto &profile : media_profiles) {
        if (profile.url == full_url) {
            return profile.token;
        }
    }

    return "";
}

template<>
struct ResourceAdapter<CameraStatistic> {
    static VmsResource toVmsResource(const CameraStatistic &stats) {
        VmsResource res;
        res.guid = stats.tuple.device_id;
        // parent_guid is the camera's primary (preferred) media server, not the node currently running it;
        // the current owner is recorded in vms_resource_assignment.
        res.parent_guid = stats.option.preferedMediaServer;
        res.name = stats.tuple.name;
        // For camera resource, use stream url as resource url if ip is not configured, so that VMS can pull stream directly from camera. 
        // If ip is configured, use http url with ip and web port as resource url for VMS to access camera info and control api, and pull stream through media server.
        std::string url;
        if (stats.option.ip.empty()) {
            auto it = stats.stream_map.find(PrimaryStream);
            if (it != stats.stream_map.end()) {
                url = it->second.full_url;
            }
            if (url.empty()) {
                auto it = stats.stream_map.find(SecondaryStream);
                if (it != stats.stream_map.end()) {
                    url = it->second.full_url;
                }
            }
        } else {
            // Same rule as the "webPage" reported to the service API (server/Manager.cpp).
            url = StrPrinter << "http://" << stats.option.ip << ":" << (stats.option.autoWebPort ? stats.option.port : stats.option.webPort);
        }
        res.url = url; 
        res.xtype_guid = getXtypeId();
        return res;
    }

    static std::vector<VmsKvPair> toKvPairs(const CameraStatistic &stats) {
        const std::string &resource_id = stats.tuple.device_id;
        const CameraOption &opt = stats.option;
        std::vector<VmsKvPair> kvs;

        auto make = [&resource_id](const std::string &key, const std::string &val) {
            VmsKvPair kv;
            kv.resource_guid = resource_id;
            kv.name  = key;
            kv.value = val;
            return kv;
        };
        // ── identity ──────────────────────────────────────────────────────
        kvs.push_back(make("name",                       opt.name));
        kvs.push_back(make("ip",                         opt.ip));
        kvs.push_back(make("port",                       std::to_string(opt.port)));
        kvs.push_back(make("manufacturer",               opt.manufacturer));
        kvs.push_back(make("model",                      opt.model));
        kvs.push_back(make("username",                   opt.username));
        kvs.push_back(make("password",                   opt.password));
        kvs.push_back(make("webPort",                    std::to_string(opt.webPort)));
        kvs.push_back(make("autoWebPort",                std::to_string(opt.autoWebPort)));

        // ── stream control ────────────────────────────────────────────────
        kvs.push_back(make("disablePrimaryStream",       std::to_string(opt.disablePrimaryStream)));
        kvs.push_back(make("disableSecondaryStream",     std::to_string(opt.disableSecondaryStream)));
        kvs.push_back(make("disableAudio",               std::to_string(opt.disableAudio)));
        kvs.push_back(make("rtpTransport",               std::to_string(opt.rtpTransport)));
        kvs.push_back(make("autoMediaPort",              std::to_string(opt.autoMediaPort)));
        kvs.push_back(make("mediaPort",                  std::to_string(opt.mediaPort)));

        // ── recording ─────────────────────────────────────────────────────
        kvs.push_back(make("enableActive",               std::to_string(opt.enableActive)));
        kvs.push_back(make("enableRecord",               std::to_string(opt.enableRecord)));
        kvs.push_back(make("doNotRecordPrimaryStream",   std::to_string(opt.doNotRecordPrimaryStream)));
        kvs.push_back(make("doNotRecordSecondaryStream", std::to_string(opt.doNotRecordSecondaryStream)));
        kvs.push_back(make("keepArchivedMaxFor",         std::to_string(opt.keepArchivedMaxFor)));
        kvs.push_back(make("keepArchivedMaxForAuto",     std::to_string(opt.keepArchivedMaxForAuto)));
        kvs.push_back(make("keepArchivedMinFor",         std::to_string(opt.keepArchivedMinFor)));
        kvs.push_back(make("keepArchivedMinForAuto",     std::to_string(opt.keepArchivedMinForAuto)));
        kvs.push_back(make("recordSchedules",            opt.recordSchedules));
        kvs.push_back(make("keepConfigProfileAndStream", std::to_string(opt.keepConfigProfileAndStream)));

        // ── failover ──────────────────────────────────────────────────────
        kvs.push_back(make("enableFailover",             std::to_string(opt.enableFailover)));
        kvs.push_back(make("preferedMediaServer",        opt.preferedMediaServer));

        // ── ptz ───────────────────────────────────────────────────────────
        kvs.push_back(make("enablePTZControl",           std::to_string(opt.enablePTZControl)));
        kvs.push_back(make("reversePanAxis",             std::to_string(opt.reversePanAxis)));
        kvs.push_back(make("reverseTiltAxis",            std::to_string(opt.reverseTiltAxis)));
        kvs.push_back(make("ptzMode",                    std::to_string(opt.ptzMode)));
        kvs.push_back(make("ptzSpeed",                   std::to_string(opt.ptzSpeed)));
        kvs.push_back(make("onvifMainProfile",           opt.onvifMainProfile));
        kvs.push_back(make("onvifSubProfile",            opt.onvifSubProfile));
        kvs.push_back(make("enableTimeSync",             std::to_string(opt.enableTimeSync)));

        // ── motion ────────────────────────────────────────────────────────
        kvs.push_back(make("enableMotion",               std::to_string(opt.enableMotion)));
        kvs.push_back(make("roiValue",                   opt.roiValue));
        kvs.push_back(make("motionDetectOnStream",       std::to_string(opt.motionDetectOnStream)));
        kvs.push_back(make("motionPreRecordSec",         std::to_string(opt.motionPreRecordSec)));
        kvs.push_back(make("motionPostRecordSec",        std::to_string(opt.motionPostRecordSec)));

        // ── additional shared camera configuration ───────────────────────
        kvs.push_back(make("sdCardSyncEnabled",          std::to_string(opt.sdCardSyncEnabled)));
        kvs.push_back(make("sdCardSyncAutoSyncEnabled",  std::to_string(opt.sdCardSyncAutoSyncEnabled)));
        kvs.push_back(make("sdCardSyncMinSegmentGapSec", std::to_string(opt.sdCardSyncMinSegmentGapSec)));
        kvs.push_back(make("sdCardSyncRetryCount",       std::to_string(opt.sdCardSyncRetryCount)));
        kvs.push_back(make("enforceWatermarkOnView",     std::to_string(opt.enforceWatermarkOnView)));
        kvs.push_back(make("enableReduceQualityOnView",  std::to_string(opt.enableReduceQualityOnView)));
        kvs.push_back(make("watermarkExcludedRoleIds",   opt.watermarkExcludedRoleIds));
        kvs.push_back(make("watermarkTemplate",          opt.watermarkTemplate));
        kvs.push_back(make("enforcePrivacyMaskOnView",   std::to_string(opt.enforcePrivacyMaskOnView)));
        kvs.push_back(make("privacyMaskExcludedRoleIds", opt.privacyMaskExcludedRoleIds));
        kvs.push_back(make("privacyMaskRegions",          opt.privacyMaskRegions));

        // ── stream urls (synced so peers know where to pull) ──────────────
        Json::Value streamUrls = Json::objectValue;
        streamUrls[std::to_string(PrimaryStream)]   = makeStreamTupleJson(stats.stream_map, PrimaryStream);
        streamUrls[std::to_string(SecondaryStream)] = makeStreamTupleJson(stats.stream_map, SecondaryStream);
        kvs.push_back(make("streamUrls", StrJsonUtils::writeJsonString(streamUrls)));

        return kvs;
    }

    static std::vector<LocalResource> toLocalProps(const CameraStatistic &stats) {
        const std::string &resource_id = stats.tuple.device_id;
        std::vector<LocalResource> props;
        auto make = [&resource_id](const std::string &key, const std::string &val) {
            LocalResource props;
            props.resource_id    = resource_id;
            props.property_name  = key;
            props.property_value = val;
            return props;
        };
        // add if needed, currently no local property for camera statistic
        // props.push_back(make("recordRootPath", stats.option.recordRootPath));
        return props;
    }

    static CameraStatistic fromVmsResource(const VmsResource &res, const std::vector<VmsKvPair> &kvs, const std::vector<LocalResource> &props) {
        CameraStatistic stats;
        // Device Tuple
        stats.tuple.vhost = DEFAULT_VHOST;
        stats.tuple.device_id = res.guid;
        stats.tuple.name = res.name;

        // Camera Option
        for (const auto &kv : kvs) {
            try {
                if (kv.name == "name") {
                    stats.option.name = kv.value;
                } else if (kv.name == "ip") {
                    stats.option.ip = kv.value;
                } else if (kv.name == "port") {
                    stats.option.port = std::stoi(kv.value);
                } else if (kv.name == "manufacturer") {
                    stats.option.manufacturer = kv.value;
                } else if (kv.name == "model") {
                    stats.option.model = kv.value;
                } else if (kv.name == "username") {
                    stats.option.username = kv.value;
                } else if (kv.name == "password") {
                    stats.option.password = kv.value;
                } else if (kv.name == "webPort") {
                    stats.option.webPort = std::stoi(kv.value);
                } else if (kv.name == "autoWebPort") {
                    stats.option.autoWebPort = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "disablePrimaryStream") {
                    stats.option.disablePrimaryStream = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "disableSecondaryStream") {
                    stats.option.disableSecondaryStream = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "disableAudio") {
                    stats.option.disableAudio = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "rtpTransport") {
                    stats.option.rtpTransport = std::stoi(kv.value);
                } else if (kv.name == "autoMediaPort") { 
                    stats.option.autoMediaPort = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "mediaPort") {
                    stats.option.mediaPort = std::stoi(kv.value);
                } else if (kv.name == "enableActive") {
                    stats.option.enableActive = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "enableRecord") {
                    stats.option.enableRecord = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "doNotRecordPrimaryStream") {
                    stats.option.doNotRecordPrimaryStream = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "doNotRecordSecondaryStream") {
                    stats.option.doNotRecordSecondaryStream = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "keepArchivedMaxFor") {
                    stats.option.keepArchivedMaxFor = std::stoull(kv.value);
                } else if (kv.name == "keepArchivedMaxForAuto") {
                    stats.option.keepArchivedMaxForAuto = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "keepArchivedMinFor") {
                    stats.option.keepArchivedMinFor = std::stoull(kv.value);
                } else if (kv.name == "keepArchivedMinForAuto") {
                    stats.option.keepArchivedMinForAuto = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "recordSchedules") {
                    stats.option.recordSchedules = kv.value;
                } else if (kv.name == "keepConfigProfileAndStream") {
                    stats.option.keepConfigProfileAndStream = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "enableFailover") {
                    stats.option.enableFailover = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "preferedMediaServer") {
                    stats.option.preferedMediaServer = kv.value;
                } else if (kv.name == "enablePTZControl") {
                    stats.option.enablePTZControl = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "enableTimeSync") {
                    stats.option.enableTimeSync = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "reversePanAxis") {
                    stats.option.reversePanAxis = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "reverseTiltAxis") {
                    stats.option.reverseTiltAxis = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "ptzMode") {
                    stats.option.ptzMode = std::stoi(kv.value);
                } else if (kv.name == "ptzSpeed") {
                    stats.option.ptzSpeed = std::stof(kv.value);
                } else if (kv.name == "onvifMainProfile") {
                    stats.option.onvifMainProfile = kv.value;
                } else if (kv.name == "onvifSubProfile") {
                    stats.option.onvifSubProfile = kv.value;
                } else if (kv.name == "enableMotion") {
                    stats.option.enableMotion = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "roiValue") {
                    stats.option.roiValue = kv.value;
                } else if (kv.name == "motionDetectOnStream") {
                    stats.option.motionDetectOnStream = std::stoi(kv.value);
                } else if (kv.name == "motionPreRecordSec") {
                    stats.option.motionPreRecordSec = std::stoi(kv.value);
                } else if (kv.name == "motionPostRecordSec") {
                    stats.option.motionPostRecordSec = std::stoi(kv.value);
                } else if (kv.name == "sdCardSyncEnabled") {
                    stats.option.sdCardSyncEnabled = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "sdCardSyncAutoSyncEnabled") {
                    stats.option.sdCardSyncAutoSyncEnabled = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "sdCardSyncMinSegmentGapSec") {
                    stats.option.sdCardSyncMinSegmentGapSec = std::stoi(kv.value);
                } else if (kv.name == "sdCardSyncRetryCount") {
                    stats.option.sdCardSyncRetryCount = std::stoi(kv.value);
                } else if (kv.name == "enforceWatermarkOnView") {
                    stats.option.enforceWatermarkOnView = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "enableReduceQualityOnView") {
                    stats.option.enableReduceQualityOnView = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "watermarkExcludedRoleIds") {
                    stats.option.watermarkExcludedRoleIds = kv.value;
                } else if (kv.name == "watermarkTemplate") {
                    stats.option.watermarkTemplate = kv.value;
                } else if (kv.name == "enforcePrivacyMaskOnView") {
                    stats.option.enforcePrivacyMaskOnView = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "privacyMaskExcludedRoleIds") {
                    stats.option.privacyMaskExcludedRoleIds = kv.value;
                } else if (kv.name == "privacyMaskRegions") {
                    stats.option.privacyMaskRegions = kv.value;
                } else if (kv.name == "streamUrls") {
                    Json::Value streamUrls_json;
                    StrJsonUtils::readJsonString(kv.value, streamUrls_json);
                    const int stream_types[] = {PrimaryStream, SecondaryStream};
                    for (int type : stream_types) {
                        auto stream = getStreamTuple(streamUrls_json[std::to_string(type)], stats.tuple, type);
                        if (!stream.empty()) {
                            stats.stream_map[type] = stream;
                        }
                    }
                }
            } catch (const std::exception &e) {
                WarnL << "Failed to parse KV pair '" << kv.name << "' = '" << kv.value << "': " << e.what();
            }
        }
        
        for (const auto &prop : props) {
            try {
                // if (prop.property_name == "recordRootPath") {
                //     stats.option.recordRootPath = prop.property_value;
                // }
            } catch (const std::exception &e) {
                WarnL << "Failed to parse local property '" << prop.property_name << "' = '" << prop.property_value << "': " << e.what();
            }
        }
        return stats;
    }

    static std::string getXtypeId() {
        auto xtypeId = ResourceTypeManager::Instance().getResourceTypeGuid(RESOURCE_TYPE_CAMERA);
        CHECK(!xtypeId.empty(), "XtypeId for Camera cannot be empty");
        return xtypeId;
    }
};

void CameraStatisticImp::postEscTask(const std::function<void()> &task) {
    // Never runs inline (callers hold _mtx).
    _esc_queue->post(task);
}

bool CameraStatisticImp::syncToEsc(bool force) {
    if (!_sync_mode || _esc_removed || !_esc_dirty) {
        return false;
    }
    auto sync_time = static_cast<uint64_t>(time(nullptr));
    if (!force && sync_time - _last_sync_time < static_cast<uint64_t>(_sync_interval_sec)) {
        TraceL << "Sync to ESC retry skipped for camera " << tuple.shortUrl() << " since last sync was at " << getTimeStr("%Y-%m-%d %H:%M:%S", _last_sync_time);
        return false;
    }
    _last_sync_time = sync_time;
    _esc_dirty = false;

    CameraStatistic params = static_cast<const CameraStatistic &>(*this);
    std::weak_ptr<CameraStatisticImp> weak_self = shared_from_this();
    postEscTask([weak_self, params]() {
        try {
            ResourceManager::Instance().addResource<CameraStatistic>(params, true);
            TraceL << "Sync to ESC for camera " << params.tuple.shortUrl();
        } catch (const std::exception &e) {
            ErrorL << "Failed to sync camera resource to ESC for camera " << params.tuple.shortUrl() << ": " << e.what();
            auto self = weak_self.lock();
            if (self) {
                std::lock_guard<std::mutex> lck(self->_mtx);
                self->_esc_dirty = true; // retried by the next save()
            }
        }
    });
    return true;
}

void CameraStatisticImp::removeFromEsc() {
    auto device_id = tuple.device_id;
    auto device_url = tuple.shortUrl();
    postEscTask([device_id, device_url]() {
        try {
            ResourceManager::Instance().removeResource(device_id);
            TraceL << "Removed camera resource from ESC for camera " << device_url;
        } catch (const std::exception &e) {
            ErrorL << "Failed to remove camera resource from ESC for camera " << device_url << ": " << e.what();
        }
    });
}

bool CameraStatisticImp::syncFromEsc(string &guid, CameraStatistic &stats) {
    DebugL << "Get camera statistic from ESC for camera with guid " << guid;
    try {
        auto ret = ResourceManager::Instance().getResource<CameraStatistic>(guid, stats);
        if (ret) {
            DebugL << "Sync from ESC for camera " << stats.tuple.shortUrl();
        } else {
            WarnL << "Camera " << guid << " not found. Sync from ESC failed";
        }
        return ret;
    } catch (const std::exception &e) {
        ErrorL << "Failed to sync camera resource from ESC for guid " << guid << ": " << e.what();
        return false;
    }
}

void CameraStatisticImp::assignResource(bool regist) {
    if (!_sync_mode || _esc_removed) {
        DebugL << "Sync mode is disabled for camera " << tuple.shortUrl() << ". Skip resource assignment";
        return;
    }
    DebugL << (regist ? "Assigning" : "Releasing") << " resource for camera " << tuple.shortUrl();
    auto device_id = tuple.device_id;
    auto device_url = tuple.shortUrl();
    bool enable_failover = option.enableFailover;
    auto prefered_server = option.preferedMediaServer;
    postEscTask([device_id, device_url, regist, enable_failover, prefered_server]() {
        try {
            auto node_id = ResourceManager::Instance().getSelfNodeId();
            if (regist) {
                auto db_guid = ResourceManager::Instance().getSelfDbGuid();
                auto assign_type = (enable_failover && prefered_server != node_id) ? ResourceAssignType::FAILOVER : ResourceAssignType::PRIMARY;
                ResourceManager::Instance().assignResource(device_id, node_id, db_guid, assign_type);
            } else {
                ResourceManager::Instance().releaseResource(device_id, node_id);
            }
        } catch (const std::exception &e) {
            ErrorL << "Failed to " << (regist ? "assign" : "release") << " resource for camera " << device_url << ": " << e.what();
        }
    });
}

void CameraStatisticImp::syncResourceStatus() {
    if (!_sync_mode || _esc_removed) {
        DebugL << "Sync mode is disabled for camera " << tuple.shortUrl() << ". Skip resource status sync";
        return;
    }
    // Priority: ONLINE (any stream live) > UNAUTHORIZED (any stream rejected credentials) > OFFLINE,
    // independent of unordered_map iteration order, so the replicated status does not flap.
    ResourceStatus state = ResourceStatus::OFFLINE;
    for (const auto &sinfo_pair : sinfo_map) {
        const auto &sinfo = sinfo_pair.second;
        if (sinfo.live) {
            state = ResourceStatus::ONLINE;
            break;
        }
        if (sinfo.status.find("Unauthorized") != string::npos) {
            state = ResourceStatus::UNAUTHORIZED;
        }
    }
    DebugL << "Sync resource status for camera " << tuple.shortUrl() << " as " << (state == ResourceStatus::ONLINE ? "ONLINE" : (state == ResourceStatus::UNAUTHORIZED ? "UNAUTHORIZED" : "OFFLINE"));
    auto device_id = tuple.device_id;
    auto device_url = tuple.shortUrl();
    postEscTask([device_id, device_url, state]() {
        try {
            ResourceManager::Instance().setResourceStatus(device_id, state);
        } catch (const std::exception &e) {
            ErrorL << "Failed to sync resource status for camera " << device_url << ": " << e.what();
        }
    });
}

void CameraStatisticImp::setSyncMode(bool enable) {
    std::lock_guard<std::mutex> lck(_mtx);
    const bool was_enabled = _sync_mode;
    _sync_mode = enable;
    DebugL << "Set sync mode for camera " << tuple.shortUrl() << " to " << (_sync_mode ? "enabled" : "disabled");
    if (enable && !was_enabled && !_esc_removed && option.enableActive) {
        // This node runs the camera. The option may already be in place (loaded from info.txt at
        // startup, and the camera skips setCameraOption when the option is unchanged), so publish
        // ownership, config and status now. An inactive local option is not published: the camera
        // is either coming back (setCameraOption follows) or owned elsewhere.
        assignResource(true);
        markEscConfigChanged();
        syncResourceStatus();
    }
}

void CameraStatisticImp::detachFromCluster() {
    std::lock_guard<std::mutex> lck(_mtx);
    if (_sync_mode) {
        // Release ownership only. The camera config in ESC belongs to the node that takes the camera
        // over; publishing this node's local "inactive" option would overwrite it.
        assignResource(false);
    }
    _sync_mode = false;
    DebugL << "Camera " << tuple.shortUrl() << " detached from cluster";
}

void CameraStatisticImp::saveStreamStatusHistory(const std::string &stream_id, const std::string &stream_url,
                                                 bool connect, const std::string &reason) {
    if (stream_id.empty()) {
        WarnL << "Cannot save local status history for an empty stream id";
        return;
    }
    // SQLite write: runs on the camera's ESC queue (ordered with its other ESC writes), not on the
    // camera poller. The event time is taken now so the gap boundaries stay exact.
    const uint64_t event_time = time(nullptr);
    postEscTask([stream_id, stream_url, connect, reason, event_time]() {
        const bool saved = connect
            ? ResourceStatusManager::Instance().onReconnected(stream_id, event_time)
            : ResourceStatusManager::Instance().onDisconnected(stream_id, reason, event_time);
        if (saved) {
            DebugL << "Saved local resource status for stream " << stream_url << ": " << (connect ? "ONLINE" : "OFFLINE") << ", reason: " << reason;
        } else {
            WarnL << "Failed to save local resource status for stream " << stream_url;
        }
    });
}

void CameraStatisticImp::saveDeviceStatusHistory(const std::string &device_id, const std::string &device_url,
                                                 bool connect, const std::string &reason) {
    // Same as saveStreamStatusHistory: ESC queue, event time taken now.
    const uint64_t event_time = time(nullptr);
    postEscTask([device_id, device_url, connect, reason, event_time]() {
        const bool saved = connect
            ? ResourceStatusManager::Instance().onReconnected(device_id, event_time)
            : ResourceStatusManager::Instance().onDisconnected(device_id, reason, event_time);
        if (saved) {
            DebugL << "Saved local resource status for camera " << device_url << ": " << (connect ? "ONLINE" : "OFFLINE") << ", reason: " << reason;
        } else {
            WarnL << "Failed to save local resource status for camera " << device_url;
        }
    });
}

} // namespace managerkit
