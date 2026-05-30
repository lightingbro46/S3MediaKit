#include "CameraStatistic.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "Local/TierStorageManager.h"
#include "Extension/Resource.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

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
    return ret;
}

static Json::Value makeDeviceCapabilitiesJson(const DeviceCapabilities &caps) {
    Json::Value ret = Json::objectValue;
    ret["isOnvifDevice"] = caps.isOnvifDevice;
    ret["onvifProfile"] = makeOnvifProfileJson(caps.onvifProfile);
    return ret;
}

static Json::Value makeDeviceStatisticJson(const DeviceStatistic &stats) {
    Json::Value ret = Json::objectValue;
    ret["connect"] = stats.connect;
    ret["status"] = stats.status;
    ret["capabilities"] = makeDeviceCapabilitiesJson(stats.device_caps);
    ret["user_presets"] = makeOnvifPTZPresetMapJson(stats.user_presets);
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
    return profile;
}

static DeviceCapabilities getDeviceCapabilities(const Json::Value &data) {
    DeviceCapabilities stats;
    stats.isOnvifDevice = !data["isOnvifDevice"].empty() ? data["isOnvifDevice"].asBool() : false;
    stats.onvifProfile = getOnvifProfile(data["onvifProfile"]);
    return stats;
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
        } else if (it["name"] == "tierStorageStats") {
            Json::Value tier_storage_json;
            StrJsonUtils::readJsonString(it["value"].asString(), tier_storage_json);
            stats.tier_storage_map[HotTier] = getTierStorageStats(tier_storage_json[HotTier]);
            stats.tier_storage_map[WarmTier] = getTierStorageStats(tier_storage_json[WarmTier]);
            stats.tier_storage_map[ColdTier] = getTierStorageStats(tier_storage_json[ColdTier]);
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

    Json::Value tier_storage_json = Json::arrayValue;
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, HotTier));
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, WarmTier));
    tier_storage_json.append(makeTierStorageStatsJson(stats.tier_storage_map, ColdTier));
    params.append(makeJsonKeyValue("tierStorageStats", StrJsonUtils::writeJsonString(tier_storage_json)));

    root["addParams"] = params;

    // created_at/updated_at
    root["created_at"] = stats.created_at;
    root["updated_at"] = stats.updated_at;

    return root.toStyledString();
}

// ################### CameraStatisticImp ###########################

CameraStatisticImp::CameraStatisticImp(const std::string &src_path, int sync_interval_sec) : _sync_interval_sec(sync_interval_sec) {
    CHECK(!src_path.empty(), "Source path cannot be empty");
    setup(src_path);
    _last_sync_time = time(nullptr);
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
        device_stats = saved_stats.device_stats;
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
    syncToEsc();
}

void CameraStatisticImp::setDeviceTuple(const DeviceTuple &input_tuple) {
    std::lock_guard<std::mutex> lck(_mtx);
    tuple = input_tuple;
    save();
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
}

void CameraStatisticImp::setCameraOption(const CameraOption &input_option) {
    std::lock_guard<std::mutex> lck(_mtx);
    option = input_option;
    save();

    if (option.enableActive) {
        // Active camera need to assign resource
        assignResource(true);
    } else if (option.enableFailover) {
        // Camera is inactive but enable failover, also need to release resource
        assignResource(false);
    }
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
    std::lock_guard<std::mutex> lck(_mtx);
    if (sinfo_map.find(stream_type) != sinfo_map.end()) {
        auto &sinfo = sinfo_map[stream_type];
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
    } else {
        WarnL << "Camera " << tuple.shortUrl() << " do not have stream type: " << stream_type << ". Ignore add stream statistic";
    }
}

void CameraStatisticImp::remove() {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        if (_file) {
            _file->remove();
            DebugL << "Removed file recorder success: " << tuple.shortUrl();
            removed = true;
        }
    }
    if (_on_remove && removed) {
        _on_remove(tuple.device_id);
    }
}

void CameraStatisticImp::addDeviceCapabilities(bool connect, string status, const DeviceCapabilities *device_caps) {
    std::lock_guard<std::mutex> lck(_mtx);
    device_stats.connect = connect;
    device_stats.status = status;
    if (device_caps) {
        device_stats.device_caps = *device_caps;
    }
    DebugL << "Device " << tuple.shortUrl() << " capabilities: Connected=" << device_stats.connect << ", Status=" << device_stats.status
           << ", isOnvifDevice=" << device_stats.device_caps.isOnvifDevice
           << ", onvifProfile.mediaProfiles.size=" << device_stats.device_caps.onvifProfile.mediaProfiles.size();
    save();
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
        ptzProfile.presetMap.emplace(preset.Token, std::move(preset));
        DebugL << "Camera " << tuple.shortUrl() << " add user preset: " << preset_token << ", name: " << preset_name
               << ", absPan: " << abs_pan << ", absTilt: " << abs_tilt << ", absZoom: " << abs_zoom
               << ". Total preset count: " << ptzProfile.presetMap.size();
    } else {
        auto it_preset = ptzProfile.presetMap.find(preset_token);
        if (it_preset != ptzProfile.presetMap.end()) {
            ptzProfile.presetMap.erase(it_preset);
            DebugL << "Camera " << tuple.shortUrl() << " remove user preset: " << preset_token
                   << ". Total preset count: " << ptzProfile.presetMap.size();
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
    std::lock_guard<std::mutex> lck(_mtx);
    auto &tier_stats = tier_storage_map[tier_type];
    if (start) {
        tier_stats.archiveStartTime = threshold;
        DebugL << "Camera " << tuple.shortUrl() << " set tier " << getTierTypeString(tier_type) << " keep start threshold: " << threshold << " seconds";
    } else {
        tier_stats.archiveEndTime = threshold;
        DebugL << "Camera " << tuple.shortUrl() << " set tier " << getTierTypeString(tier_type) << " keep end threshold: " << threshold << " seconds";
    }
    save();
}

template<>
struct ResourceAdapter<CameraStatistic> {
    static VmsResource toVmsResource(const CameraStatistic &stats) {
        VmsResource res;
        res.guid = stats.tuple.device_id;
        res.parent_guid = stats.option.preferedMediaServer; // Use preferedMediaServer as parent_guid to associate camera resource with media server resource in VMS
        res.name = stats.tuple.name;
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

        // ── motion ────────────────────────────────────────────────────────
        kvs.push_back(make("enableMotion",               std::to_string(opt.enableMotion)));
        kvs.push_back(make("roiValue",                   opt.roiValue));
        kvs.push_back(make("motionDetectOnStream",       std::to_string(opt.motionDetectOnStream)));
        kvs.push_back(make("motionPreRecordSec",         std::to_string(opt.motionPreRecordSec)));
        kvs.push_back(make("motionPostRecordSec",        std::to_string(opt.motionPostRecordSec)));

        // ── stream urls (synced so peers know where to pull) ──────────────
        Json::Value streamUrls = Json::objectValue;
        streamUrls[PrimaryStream]   = makeStreamTupleJson(stats.stream_map, PrimaryStream);
        streamUrls[SecondaryStream] = makeStreamTupleJson(stats.stream_map, SecondaryStream);
        kvs.push_back(make("streamUrls", StrJsonUtils::writeJsonString(streamUrls)));

        // ── stream statistic (synced so peers know where to pull) ──────────────
        Json::Value mediaStreams = Json::objectValue;
        mediaStreams[PrimaryStream] = makeStreamStatisticJson(stats.sinfo_map, PrimaryStream);
        mediaStreams[SecondaryStream] = makeStreamStatisticJson(stats.sinfo_map, SecondaryStream);;
        kvs.push_back(make("mediaStreams", StrJsonUtils::writeJsonString(mediaStreams)));

        // ── storage info (synced so peers know where to pull) ──────────────
        Json::Value storage_info = Json::objectValue;
        auto bm_json = makeBookmarkStatsJson(stats.bm);
        storage_info["bookmarkStats"] = bm_json;

        auto motion_json = makeMotionStorageStatsJson(stats.motion_stats);
        storage_info["motionStats"] = motion_json;

        Json::Value stream_storage_json = Json::arrayValue;
        auto it_primary = stats.stream_map.find(PrimaryStream);
        if (it_primary != stats.stream_map.end()) {
            Json::Value stream_json = Json::objectValue;
            auto stream_id = it_primary->second.stream_id;
            stream_json["key"] = stream_id;
            stream_json["value"] = Json::objectValue;
            if (stats.storage_map.find(stream_id) != stats.storage_map.end()) {
                stream_json["value"] = makeStreamStorageStatsJson(stats.storage_map, stats.stream_map, PrimaryStream);
            }
            stream_storage_json.append(stream_json);
        }

        auto it_secondary = stats.stream_map.find(SecondaryStream);
        if (it_secondary != stats.stream_map.end()) {
            Json::Value stream_json = Json::objectValue;
            auto stream_id = it_secondary->second.stream_id;
            stream_json["key"] = stream_id;
            stream_json["value"] = Json::objectValue;
            if (stats.storage_map.find(stream_id) != stats.storage_map.end()) {
                stream_json["value"] = makeStreamStorageStatsJson(stats.storage_map, stats.stream_map, SecondaryStream);
            }
            stream_storage_json.append(stream_json);
        }
        storage_info["streamStorageInfos"] = stream_storage_json;
        storage_info["lastUpdateTimeMs"] = (Json::UInt64)(stats.updated_at * 1000);

        kvs.push_back(make("storageInfos", StrJsonUtils::writeJsonString(storage_info)));

        return kvs;
    }

    static std::vector<LocalResource> toLocalProps(const CameraStatistic &stats) {
        const std::string &resource_id = stats.tuple.device_id;
        std::vector<LocalResource> props;
        // auto make = [&resource_id](const std::string &key, const std::string &val) {
        //     LocalResource props;
        //     props.resource_id    = resource_id;
        //     props.property_name  = key;
        //     props.property_value = val;
        //     return props;
        // };
        // add if needed, currently no local property for camera statistic
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
                stats.option.keepArchivedMaxFor = std::stoi(kv.value);
            } else if (kv.name == "keepArchivedMaxForAuto") {
                stats.option.keepArchivedMaxForAuto = static_cast<bool>(std::stoi(kv.value));
            } else if (kv.name == "keepArchivedMinFor") {
                stats.option.keepArchivedMinFor = std::stoi(kv.value);
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
            } else if (kv.name == "streamUrls") {
                Json::Value streamUrls_json;
                StrJsonUtils::readJsonString(kv.value, streamUrls_json);
                stats.stream_map[PrimaryStream] = getStreamTuple(streamUrls_json[PrimaryStream], stats.tuple, PrimaryStream);
                stats.stream_map[SecondaryStream] = getStreamTuple(streamUrls_json[SecondaryStream], stats.tuple, SecondaryStream);
            } else if (kv.name == "mediaStreams") {
                Json::Value mediaStreams_json;
                StrJsonUtils::readJsonString(kv.value, mediaStreams_json);
                stats.sinfo_map[PrimaryStream] = getStreamStatistic(mediaStreams_json[PrimaryStream]);
                stats.sinfo_map[SecondaryStream] = getStreamStatistic(mediaStreams_json[SecondaryStream]);
            } else if (kv.name == "storageInfos") {
                Json::Value storage_info_json;
                StrJsonUtils::readJsonString(kv.value, storage_info_json);
                stats.bm = getBookmarkStats(storage_info_json["bookmarkStats"]);
                stats.motion_stats = getMotionStorageStats(storage_info_json["motionStats"]);
                for (const auto &stream_storage_json : storage_info_json["streamStorageInfos"]) {
                    auto stream_id = stream_storage_json["key"].asString();
                    auto storage_stats = getStreamStorageStats(stream_storage_json["value"]);
                    stats.storage_map[stream_id] = storage_stats;
                }
            }
        }
        return stats;
    }

    static std::string getXtypeId() {
        return ResourceTypeManager::Instance().getResourceTypeGuid("Camera");
    }
};

void CameraStatisticImp::syncToEsc() {
    auto sync_time = time(nullptr);
    if (sync_time - _last_sync_time < static_cast<uint64_t>(_sync_interval_sec)) {
        TraceL << "Camera statistic sync to ESC skipped for camera " << tuple.shortUrl() << " since last sync was at " << getTimeStr("%Y-%m-%d %H:%M:%S", _last_sync_time);
        return;
    }
    auto params = getParams();
    ResourceManager::Instance().addResource<CameraStatistic>(params, true);
    _last_sync_time = time(nullptr);
    DebugL << "Camera statistic sync to ESC for camera " << params.tuple.shortUrl() << " at " << getTimeStr("%Y-%m-%d %H:%M:%S", _last_sync_time);
}

bool CameraStatisticImp::syncFromEsc(string &guid, CameraStatistic &stats) {
    DebugL << "Get camera statistic from ESC for camera with guid " << guid;
    auto ret = ResourceManager::Instance().getResource<CameraStatistic>(guid, stats);
    if (ret) {
        DebugL << "Camera statistic sync from ESC for camera " << stats.tuple.shortUrl();
    } else {
        WarnL << "Camera statistic from ESC for camera " << guid << " not found";
    }
    return ret;
}

void CameraStatisticImp::assignResource(bool regist) {
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    if (regist) {
        auto assign_type = (option.enableFailover && option.preferedMediaServer != mediaServerId) ? ResourceAssignType::FAILOVER : ResourceAssignType::PRIMARY;
        ResourceManager::Instance().assignResource(tuple.device_id, mediaServerId, assign_type);
    } else {
        ResourceManager::Instance().releaseResource(tuple.device_id, mediaServerId);
    }
}

void CameraStatisticImp::syncResourceStatus() {
    ResourceStatus state = ResourceStatus::OFFLINE;
    for (const auto &sinfo_pair : sinfo_map) {
        const auto &sinfo = sinfo_pair.second;
        if (sinfo.live) {
            state = ResourceStatus::ONLINE;
            break;
        }
        if (sinfo.status.find("Unauthorized") != string::npos) {
            state = ResourceStatus::UNAUTHORIZED;
        } else {
            state = ResourceStatus::OFFLINE;
        }
    }
    ResourceManager::Instance().setResourceStatus(tuple.device_id, state);
}

} // namespace managerkit