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
static Json::Value makeStreamStorageStatsJson(unordered_map<int, StreamStorageStats> storage_map, int stream_type) {
    Json::Value ret = Json::objectValue;
    if (storage_map.find(stream_type) != storage_map.end()) {
        ret["archiveIndexRecordCount"] = storage_map[stream_type].archiveIndexRecordCount;
        ret["archiveSizeB"] = storage_map[stream_type].archiveSizeB;
        ret["archiveStartTime"] = storage_map[stream_type].archiveStartTime;
        ret["archiveEndTime"] = storage_map[stream_type].archiveEndTime;
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
static Json::Value makeStreamStatisticJson(unordered_map<int, StreamStatistic> stats_map, int stream_type) {
    Json::Value ret = Json::objectValue;
    if (stats_map.find(stream_type) != stats_map.end()) {
        ret["live"] = stats_map[stream_type].live;
        ret["status"] = stats_map[stream_type].status;
        ret["byte_speed"] = stats_map[stream_type].byte_speed;
        ret["has_video"] = stats_map[stream_type].has_video;
        ret["vcodec"] = stats_map[stream_type].vcodec;
        ret["width"] = stats_map[stream_type].width;
        ret["height"] = stats_map[stream_type].height;
        ret["bitrate"] = stats_map[stream_type].bitrate;
        ret["fps"] = stats_map[stream_type].fps;
        ret["has_audio"] = stats_map[stream_type].has_audio;
        ret["acodec"] = stats_map[stream_type].acodec;
        ret["sample_rate"] = stats_map[stream_type].sample_rate;
        ret["channel_no"] = stats_map[stream_type].channel_no;
        ret["sample_bit"] = stats_map[stream_type].sample_bit;
    }
    return ret;
}

static StreamStatistic getStreamStatistic(const Json::Value &data) {
    StreamStatistic stats;
    stats.live = data["live"].asBool();
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
    ret["ptzCapabilities"] = stats.ptzCapabilities;
    return ret;
}

static DeviceCapabilities getDeviceCapabilities(const Json::Value &data) {
    DeviceCapabilities stats;
    stats.ptzCapabilities = data["ptzCapabilities"].asBool();
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
    option.recordScheduler = ret["recordScheduler"].asString();
    option.enableFailover = ret["enableFailover"].asBool();
    option.preferedMediaServer = ret["preferedMediaServer"].asString();
    option.enablePTZControl = ret["enablePTZControl"].asBool();
    stats.option = option;

    // stream tuple map
    unordered_map<int, StreamTuple> stream_map;
    auto streamUrlsString = ret["streamUrls"].asString();
    Json::Value streamUrlsJson;
    readJsonString(streamUrlsString, streamUrlsJson);
    stream_map[PrimaryStream] = getStreamTuple(streamUrlsJson[PrimaryStream], info);
    stream_map[SecondaryStream] = getStreamTuple(streamUrlsJson[SecondaryStream], info);
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
            stats.storage_map[PrimaryStream] = getStreamStorageStats(storage_json[PrimaryStream]);
            stats.storage_map[SecondaryStream] = getStreamStorageStats(storage_json[SecondaryStream]);
        } else if (it["name"] == "streamStatisticInfos") {
            Json::Value stream_stats_json;
            readJsonString(it["value"].asString(), stream_stats_json);
            stats.sinfo_map[PrimaryStream] = getStreamStatistic(stream_stats_json[PrimaryStream]);
            stats.sinfo_map[SecondaryStream] = getStreamStatistic(stream_stats_json[SecondaryStream]);
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
    root["recordScheduler"] = stats.option.recordScheduler;
    root["enableFailover"] = stats.option.enableFailover;
    root["preferedMediaServer"] = stats.option.preferedMediaServer;
    root["enablePTZControl"] = stats.option.enablePTZControl;

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
    storage_json.append(makeStreamStorageStatsJson(stats.storage_map, PrimaryStream));
    storage_json.append(makeStreamStorageStatsJson(stats.storage_map, SecondaryStream));
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

CameraStatisticImp::CameraStatisticImp(const CameraInfo &info_, const unordered_map<int, StreamTuple> &stream_map_) {
    info = info_;
    stream_map = stream_map_;
    setup();
}

CameraStatisticImp::~CameraStatisticImp() {}

void CameraStatisticImp::setup() {
    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath)
    GET_CONFIG(string, app_name, Record::kAppName)
    auto record_path = File::absolutePath(app_name, mp4_save_path);
    auto file_path = record_path + "/" + info.device_id + "/info.txt";
    _file = std::make_shared<FileRecorder<CameraStatistic, CameraStatisticHelper>>(file_path);
    _file->empty() ? save() : load();
}

void CameraStatisticImp::load() {
    CameraStatistic saved_stats;
    if (_file->load(saved_stats)) {
        // Manually assign fields from stats to this, except camera_info and stream_map
        option = saved_stats.option;
        bm = saved_stats.bm;
        storage_map = saved_stats.storage_map;
        sinfo_map = saved_stats.sinfo_map;
        created_at = saved_stats.created_at;
        updated_at = saved_stats.updated_at;

        if (stream_map.find(PrimaryStream) == stream_map.end()) {
            // Clear statistic if stream type do not exist
            storage_map[PrimaryStream] = StreamStorageStats();
            sinfo_map[PrimaryStream] = StreamStatistic();
        }
        if (stream_map.find(SecondaryStream) == stream_map.end()) {
            // Clear statistic if stream type do not exist
            storage_map[SecondaryStream] = StreamStorageStats();
            sinfo_map[SecondaryStream] = StreamStatistic();
        }
    }
}

void CameraStatisticImp::save() {
    if (created_at == 0) {
        created_at = time(nullptr);
    }
    updated_at = time(nullptr);
    _file->save(static_cast<const CameraStatistic &>(*this));
}

void CameraStatisticImp::setCameraOption(const CameraOption &option_) {
    std::lock_guard<std::recursive_mutex> lck(_mtx_stats);
    option = option_;
    save();
    onSetCameraOption(option_);
}

const CameraOption &CameraStatisticImp::getCameraOption() {
    std::lock_guard<std::recursive_mutex> lck(_mtx_stats);
    return option;
}

void CameraStatisticImp::addArchiveSize(string stream_id, size_t size, uint64_t archived_start_time, uint64_t archived_end_time, bool add) {
    std::lock_guard<std::recursive_mutex> lck(_mtx_stats);
    // Do not process time blocks with an end time earlier than the info file creation time, so that old media data does not need to be re-aggregated
    if (archived_end_time <= created_at) {
        DebugL << "Time block has end time (" << archived_end_time << ") less than or equal created_at of file ("<< created_at <<"). Ignore" ;
        return;
    }
    int stream_type = StreamMax;
    for (const auto &it : stream_map) {
        if (it.second.stream_id == stream_id) {
            stream_type = it.first;
        }
    }
    if (storage_map.find(stream_type) != storage_map.end()) {
        auto &storage = storage_map[stream_type];
        if (add) {
            storage.archiveSizeB += size;
            storage.archiveIndexRecordCount++;
            if (storage.archiveStartTime == 0) {
                storage.archiveStartTime = archived_start_time;
            }
            storage.archiveEndTime = archived_end_time;
            DebugL << "Add archived size: " << format_bytes_human_readable(size)
                   << ". Total archived size: " << format_bytes_human_readable(storage.archiveSizeB)
                   << ". First archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveStartTime)
                   << ". Last archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveEndTime);
        } else {
            if (storage.archiveSizeB >= size) {
                storage.archiveSizeB -= size;
            } else {
                storage.archiveSizeB = 0;
            }
            if (storage.archiveIndexRecordCount > 0) {
                storage.archiveIndexRecordCount--;
            }
            if (storage.archiveIndexRecordCount == 0) {
                // record count is equal 0, reset archiveStartTime and archiveEndTime equal 0 to indicate no data
                storage.archiveStartTime = 0;
                storage.archiveEndTime = 0;
            } else {
                // Note: Use the end time block for approximate statistics, not completely accurate. Use the TimeQuery::getFirstBlock function to get the exact number.
                storage.archiveStartTime = archived_end_time;
            }
            DebugL << "Subtract archived size: " << format_bytes_human_readable(size)
                   << ". Total archived size: " << format_bytes_human_readable(storage.archiveSizeB)
                   << ". First archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveStartTime)
                   << ". Last archived time: " << getTimeStr("%Y-%m-%d %H:%M:%S", storage.archiveEndTime);
        }
        save();
    } else {
        WarnL << "Camera storage do not have stream: " << stream_id;
    }
}

void CameraStatisticImp::addBookmarkCount(uint64_t bm_created_at, size_t size, bool add) {
    std::lock_guard<std::recursive_mutex> lck(_mtx_stats);
    // Do not process bookmark with an creation time earlier than the info file creation time, so that old media data does not need to be re-aggregated
    if (bm_created_at <= created_at) {
        DebugL << "Bookmark has creation time (" << bm_created_at << ") less than or equal created_at of file ("<< created_at <<"). Ignore" ;
        return;
    }
    if (add) {
        bm.recordCount ++;
        bm.recordAverageSizeB += size;
        DebugL << "Add bookmark count: 1. Total bookmark count: " << bm.recordCount;

    } else {
        if (bm.recordCount > 0) {
            bm.recordCount--;
        }
        if (bm.recordAverageSizeB >= size) {
            bm.recordAverageSizeB -= size;
        } else {
            bm.recordAverageSizeB = 0;
        }
        DebugL << "Subtract bookmark count: 1. Total bookmark count: " << bm.recordCount;
    }
    save();
}

CameraStatistic CameraStatisticImp::getParams() {
    std::lock_guard<std::recursive_mutex> lck(_mtx_stats);
    return static_cast<const CameraStatistic &>(*this);
}

void CameraStatisticImp::addCameraArchiveSize(const TimeBlock &block, bool add) {
    DeviceTuple tuple;
    tuple.vhost = DEFAULT_VHOST;
    tuple.device_id = block.app();

    string stream_id = block.stream();
    size_t block_size = block.file_size();
    uint64_t start_stamp = block.start_time();
    uint64_t end_stamp = block.start_time() + block.time_len();

    auto ret = DeviceSource::find(CAMERA_SCHEMA, tuple.vhost, tuple.device_id);
    if (ret) {
        auto ptr = dynamic_pointer_cast<CameraStatisticImp>(ret);
        if (ptr) {
            ptr->addArchiveSize(stream_id, block_size, start_stamp, end_stamp, add);
            return;
        }
    }
    WarnL << "Device not found: " << tuple.shortUrl();
}

void CameraStatisticImp::addCameraBookmarkCount(const std::string &camera_id, uint64_t created_at, bool add) {
    DeviceTuple tuple;
    tuple.vhost = DEFAULT_VHOST;
    tuple.device_id = camera_id;

    auto ret = DeviceSource::find(CAMERA_SCHEMA, tuple.vhost, tuple.device_id);
    if (ret) {
        auto ptr = std::dynamic_pointer_cast<CameraStatisticImp>(ret);
        if (ptr) {
            ptr->addBookmarkCount(created_at, 0, add);
            return;
        }
    }
    WarnL << "Device not found: " << tuple.shortUrl();
}

void CameraStatisticImp::addStreamStatistic(int stream_type, bool live, string status, const TranslationInfo *info) {
    std::lock_guard<std::recursive_mutex> lck(_mtx_stats);
    if (sinfo_map.find(stream_type) != sinfo_map.end()) {
        auto &sinfo = sinfo_map[stream_type];
        sinfo.live = live;
        sinfo.status = status;
        sinfo.byte_speed = live && info ? info->byte_speed : 0;
        if (live && info) {
            for (const auto &it : info->stream_info) {
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
        DebugL << "Stream statistic: Live=" << sinfo.live << ". Status=" << sinfo.status << ". Byte_speed=" << sinfo.byte_speed << " bytes/s";
    } else {
        WarnL << "Stream statistic do not have stream type: " << stream_type;
    }
}

void CameraStatisticImp::remove() {
    std::lock_guard<std::recursive_mutex> lck(_mtx_stats);
    if (_file) {
        _file->remove();
        DebugL << "Removed file recorder success: " << info.shortUrl();
    }
}

void CameraStatisticImp::addDeviceCapabilities(bool enable_ptz) {
    std::lock_guard<std::recursive_mutex> lck(_mtx_stats);
    device_caps.ptzCapabilities = enable_ptz;
    DebugL << "Device capabilities: PTZ=" << device_caps.ptzCapabilities;
}

} // namespace managerkit
