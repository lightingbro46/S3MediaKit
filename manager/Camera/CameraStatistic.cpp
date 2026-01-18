#include "CameraStatistic.h"
#include "Common/config.h"
#include "json/json.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

static bool readJsonString(const string &json_str, Json::Value &out) {
    // parse json string to json var
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    Json::Value data;
    string errs;

    unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(json_str.c_str(), json_str.c_str() + json_str.size(), &data, &errs)) {
        WarnL << "Parse json string failed: " << errs;
        return false;
    }
    // get stream information from json var
    TraceL << "Json data: " << data.toStyledString();
    out = data;
    return true;
}

static string writeJsonString(const Json::Value &in) {
    // parse json string to json var
    Json::StreamWriterBuilder writer;
    string output = Json::writeString(writer, in);
    return output;
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
static Json::Value makeDeviceCapabilitiesJson(const DeviceCapabilities stats) {
    Json::Value ret = Json::objectValue;
    ret["connect"] = stats.connect;
    ret["status"] = stats.status;
    ret["ptzCapabilities"] = stats.ptzCapabilities;
    return ret;
}

static DeviceCapabilities getDeviceCapabilities(const Json::Value &data) {
    DeviceCapabilities stats;
    stats.connect = !data["connect"].empty() ? data["connect"].asBool() : false;
    stats.status = !data["status"].empty() ? data["status"].asString() : "";
    stats.ptzCapabilities = !data["ptzCapabilities"].empty() ? data["ptzCapabilities"].asBool() : false;
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

static StreamTuple getStreamTuple(const Json::Value &data, const CameraInfo &info) {
    StreamTuple tuple;
    tuple.vhost = info.vhost;
    tuple.device_id = info.device_id;
    tuple.stream_id = data["id"].asString();
    tuple.full_url = data["url"].asString();
    return tuple;
}


// ################### CameraStatisticHelper ###########################

bool CameraStatisticHelper::getParams(const string &json_str, CameraStatistic &stats) {
    Json::Value ret;
    if (!readJsonString(json_str, ret)) {
        WarnL << "Parse json string failed";
        return false;
    }
    CameraInfo info;
    info.vhost = ret["vhost"].asString();
    info.device_id = ret["id"].asString();
    info.name = ret["name"].asString();
    info.ip = ret["ip"].asString();
    info.port = ret["port"].asInt();
    info.manufacturer = ret["manufacturer"].asString();
    info.model = ret["model"].asString();
    info.username = ret["username"].asString();
    info.password = ret["password"].asString();
    stats.info = info;

    CameraOption option;
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
    stats.option = option;

    // stream tuple map
    unordered_map<int, StreamTuple> stream_map;
    auto streamUrlsString = ret["streamUrls"].asString();
    Json::Value streamUrlsJson;
    readJsonString(streamUrlsString, streamUrlsJson);
    auto primary_stream = getStreamTuple(streamUrlsJson[PrimaryStream], info);
    if (!primary_stream.empty()) {
        stream_map[PrimaryStream] = primary_stream;
    }
    auto secondary_stream = getStreamTuple(streamUrlsJson[SecondaryStream], info);
    if (!secondary_stream.empty()) {
        stream_map[SecondaryStream] = secondary_stream;
    }
    stats.stream_map = stream_map;

    // add params
    for (const auto &it : ret["addParams"]) {
        if (it["name"] == "bookmarkStats") {
            Json::Value bm_json;
            readJsonString(it["value"].asString(), bm_json);
            stats.bm = getBookmarkStats(bm_json);
        } else if (it["name"] == "streamStorageInfos") {
            Json::Value storage_json;
            readJsonString(it["value"].asString(), storage_json);
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
            readJsonString(it["value"].asString(), stream_stats_json);
            if (!primary_stream.empty()) {
                stats.sinfo_map[PrimaryStream] = getStreamStatistic(stream_stats_json[PrimaryStream]);
            }
            if (!secondary_stream.empty()) {
                stats.sinfo_map[SecondaryStream] = getStreamStatistic(stream_stats_json[SecondaryStream]);
            }
        } else if (it["name"] == "deviceCapabilities") {
            Json::Value device_caps_json;
            readJsonString(it["value"].asString(), device_caps_json);
            stats.device_caps = getDeviceCapabilities(device_caps_json);
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
    root["vhost"] = stats.info.vhost;
    root["id"] = stats.info.device_id;
    root["name"] = stats.info.name;
    root["ip"] = stats.info.ip;
    root["port"] = stats.info.port;
    root["manufacturer"] = stats.info.manufacturer;
    root["model"] = stats.info.model;
    root["username"] = stats.info.username;
    root["password"] = stats.info.password;

    // camera option
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

    // stream tuple map
    Json::Value streamUrls = Json::arrayValue;
    streamUrls[PrimaryStream] = makeStreamTupleJson(stats.stream_map, PrimaryStream);
    streamUrls[SecondaryStream] = makeStreamTupleJson(stats.stream_map, SecondaryStream);
    root["streamUrls"] = writeJsonString(streamUrls);
    // add params
    Json::Value params = Json::arrayValue;
    Json::Value bm_json = makeBookmarkStatsJson(stats.bm);
    params.append(makeJsonKeyValue("bookmarkStats", writeJsonString(bm_json)));

    Json::Value storage_json = Json::arrayValue;
    storage_json.append(makeStreamStorageStatsJson(stats.storage_map, stats.stream_map, PrimaryStream));
    storage_json.append(makeStreamStorageStatsJson(stats.storage_map, stats.stream_map, SecondaryStream));
    params.append(makeJsonKeyValue("streamStorageInfos", writeJsonString(storage_json)));

    Json::Value stream_stats_json = Json::arrayValue;
    stream_stats_json.append(makeStreamStatisticJson(stats.sinfo_map, PrimaryStream));
    stream_stats_json.append(makeStreamStatisticJson(stats.sinfo_map, SecondaryStream));
    params.append(makeJsonKeyValue("streamStatisticInfos", writeJsonString(stream_stats_json)));

    Json::Value device_caps_json = makeDeviceCapabilitiesJson(stats.device_caps);
    params.append(makeJsonKeyValue("deviceCapabilities", writeJsonString(device_caps_json)));

    root["addParams"] = params;

    // created_at/updated_at
    root["created_at"] = stats.created_at;
    root["updated_at"] = stats.updated_at;

    return root.toStyledString();
}

// ################### CameraStatisticImp ###########################

CameraStatisticImp::CameraStatisticImp(const std::string &src_path) {
    CHECK(!src_path.empty(), "Source path cannot be empty");
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
        info = saved_stats.info;
        stream_map = saved_stats.stream_map;
        option = saved_stats.option;
        bm = saved_stats.bm;
        storage_map = saved_stats.storage_map;
        sinfo_map = saved_stats.sinfo_map;
        device_caps = saved_stats.device_caps;
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
}

void CameraStatisticImp::setCameraInfo(const CameraInfo &input_info) {
    std::lock_guard<std::mutex> lck(_mtx);
    info = input_info;
    save();
}

void CameraStatisticImp::setStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto it_primary = input_stream_map.find(PrimaryStream);
    if (it_primary != input_stream_map.end()) {
        auto tuple = it_primary->second;
        if (!tuple.empty()) {
            // Ensure primary stream exist in storage_map and sinfo_map
            if (storage_map.find(tuple.stream_id) == storage_map.end()) {
                storage_map[tuple.stream_id] = StreamStorageStats();
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
        auto tuple = it_secondary->second;
        if (!tuple.empty()) {
            // Ensure secondary stream exist in storage_map and sinfo_map
            if (storage_map.find(tuple.stream_id) == storage_map.end()) {
                storage_map[tuple.stream_id] = StreamStorageStats();
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
            DebugL << "Stream " << info.shortUrl() << "/" << stream_id << " add archived size: " << format_bytes_human_readable(size) << ", count: " << count
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
            DebugL << "Stream " << info.shortUrl() << "/" << stream_id << " subtract archived size: " << format_bytes_human_readable(size) << ", count: " << count
                   << ". Total archived size: " << format_bytes_human_readable(storage.archiveSizeB)
                   << ". Total archived count: " << storage.archiveIndexRecordCount
                   << ". First archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveStartTime)
                   << ". Last archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveEndTime);
        }
        save();
    } else {
        WarnL << "Camera " << info.shortUrl() << " do not have stream: " << stream_id << ". Ignore archive size statistic";
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
        DebugL << "Camera " << info.shortUrl() << " add bookmark count: 1. Total bookmark count: " << bm.recordCount;

    } else {
        if (bm.recordCount > 0) {
            bm.recordCount--;
        }
        if (bm.recordAverageSizeB >= size) {
            bm.recordAverageSizeB -= size;
        } else {
            bm.recordAverageSizeB = 0;
        }
        DebugL << "Camera " << info.shortUrl() << " subtract bookmark count: 1. Total bookmark count: " << bm.recordCount;
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
    } else {
        WarnL << "Camera " << info.shortUrl() << " do not have stream type: " << stream_type << ". Ignore add stream statistic";
    }
}

void CameraStatisticImp::remove() {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        if (_file) {
            _file->remove();
            DebugL << "Removed file recorder success: " << info.shortUrl();
            removed = true;
        }
    }
    if (_on_remove && removed) {
        _on_remove(info.device_id);
    }
}

void CameraStatisticImp::addDeviceCapabilities(bool connect, string status, bool enable_ptz) {
    std::lock_guard<std::mutex> lck(_mtx);
    device_caps.connect = connect;
    device_caps.status = status;
    device_caps.ptzCapabilities = enable_ptz;
    DebugL << "Device " << info.shortUrl() << " capabilities: Connected=" << device_caps.connect << ", Status=" << device_caps.status << ", PTZ=" << device_caps.ptzCapabilities;
    save();
}

} // namespace managerkit