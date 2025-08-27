#include <cmath>
#include <ctime>
#include <sstream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Record/Recorder.h"
#include "Local/TimeRecorder.h"
#include "Local/TimeQuery.h"
#include "Storage/MigrationHistory.h"
#include "Local/StorageManager.h"
#include "Server/GlobalMonitor.h"
#include "Camera/CameraManager.h"
#include "Manager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace managerkit {

namespace Manager {
#define MANAGER_FIELD "manager."
const string kMediaServerDomain = MANAGER_FIELD"mediaServerDomain";
const string kMaxAllowedDevices = MANAGER_FIELD"maxAllowedDevices";
const string kServerLocationId = MANAGER_FIELD"serverLocationId";
const string kEnableFailover = MANAGER_FIELD"enableFailover";
const string kEnableAuthorize = MANAGER_FIELD"enableAuthorize";

static onceToken token([]() {
    mINI::Instance()[kMediaServerDomain] = "";
    mINI::Instance()[kMaxAllowedDevices] = 256;
    mINI::Instance()[kServerLocationId] = 1;
    mINI::Instance()[kEnableFailover] = false;
    mINI::Instance()[kEnableAuthorize] = false;
});
} // namespace Manager

} // namespace managerkit

void enforceStoragePolicy() {
    StorageManager::Instance().start();
    DebugL << "Storage manager has been started monitoring";
}

static void *manager_hook_tag = nullptr;

void installManagerHook () {

#ifdef ENABLE_MP4
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        DebugL << "Record mp4 file " << info.app << " " << info.stream << " " << info.start_time << " " << info.time_len << " " << info.file_path;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(round(info.time_len));
        block.set_file_size(info.file_size);
        block.set_file_path(info.file_path);

        TimeRecorder::Instance().inputBlock(block);
    });
#endif // ENABLE_MP4

#ifdef ENABLE_MKV
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastRecordMKV, [](BroadcastRecordMKVArgs) {
        TraceL << "Record mkv file " << info.app << " " << info.stream << " " << info.start_time << " " << info.time_len << " " << info.file_path;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(round(info.time_len));
        block.set_file_size(info.file_size);
        block.set_file_path(info.file_path);

        TimeRecorder::Instance().inputBlock(block);
    });
#endif // ENABLE_MKV

#if defined(ENABLE_MP4) || defined(ENABLE_MKV)
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastMediaSeeked, [](BroadcastMediaSeekedArgs) {
        auto infos = split(args.stream, "/");
        MediaTuple tuple = { args.vhost, infos[0], infos[1], "" };
        auto query = make_shared<TimeQuery>(tuple);
        int64_t offset = query->getOffsetOfDate(stamp);
        invoker(offset);
    });
#endif // defined(ENABLE_MP4) || defined(ENABLE_MKV)

    enforceStoragePolicy();
}

void unInstallManagerHook() {
    // turn off and clear all camera
    CameraManager::Instance().clear();

    // Note: Comment the following code in order to save last segments when program exit
    // NoticeCenter::Instance().delListener(&manager_hook_tag);
}

void migrateDatabase() {
    TraceL << "Prepare migrating local media server database";
    auto localDbMigrate = make_shared<MigrationHistoryImp>(Database::kMediaServerDb);
    GET_CONFIG(string, mserverUpdateSavePath, Database::kMServerMigrationSavePath)
    localDbMigrate->migrate(mserverUpdateSavePath);

    TraceL << "Prepare migrating edge storage database";
    auto escDbMigrate = make_shared<MigrationHistoryImp>(Database::kEdgeStorageControllerDb);
    GET_CONFIG(string, escUpdateSavePath, Database::kESCMigrationSavePath)
    escDbMigrate->migrate(escUpdateSavePath);
}

static void fromJson(CameraInfo &info, const Json::Value &data) {
    string project_id = data["project_id"].asString();
    string device_id = data["device_id"].asString();
    string name = data["name_device"].asString();
    string manufacturer = data["manufacturer"].asString();
    string model = data["model"].asString();
    string username = data["username"].asString();
    string password = data["password"].asString();
    string ip = data["address"].asString();
    int port = data["http_port"].asInt();
    
    info.vhost = DEFAULT_VHOST;
    info.device_id = device_id;
    info.name = name;
    info.manufacturer = manufacturer;
    info.model = model;
    info.ip = ip;
    info.port = port;
    info.username = username;
    info.password = password;
}

static void fromJson(CameraOption &option, const Json::Value &data) {
    bool enable_camera = data["is_enable"].asBool();
    bool enable_recording = data["enable_recording"].asBool();
    bool do_not_record_primary_stream = data["do_not_record_primary_stream"].asBool();
    bool do_not_record_secondary_stream = data["do_not_record_secondary_stream"].asBool();
    bool keep_archived_min_for_auto = data["keep_archived_min_for_auto"].asBool();
    int keep_archived_min_for = data["keep_archived_min_for"].asInt();
    bool keep_archived_max_for_auto = data["keep_archived_max_for_auto"].asBool();
    int keep_archived_max_for = data["keep_archived_max_for"].asInt();
    int media_port = data["media_port"].asInt();
    bool media_port_auto = data["media_port_auto"].asBool();
    int rtp_transport = data["rtp_transport"].asInt();
    
    // option.enableActive = enable_camera;
    // option.enableRecord = enable_recording;
    // option.doNotRecordPrimaryStream = do_not_record_primary_stream;
    // option.doNotRecordSecondaryStream = do_not_record_secondary_stream;
    // option.keepArchivedMinForAuto = keep_archived_min_for_auto;
    // option.keepArchivedMinFor = keep_archived_min_for;
    // option.keepArchivedMaxForAuto = keep_archived_max_for_auto;
    // option.keepArchivedMaxFor = keep_archived_max_for;
    // option.mediaPort = media_port;
    // option.autoMediaPort = media_port_auto;
    // option.rtpTransport = rtp_transport;

    option.enableActive = enable_camera;
    option.enableRecord = true;
    option.doNotRecordPrimaryStream = false;
    option.doNotRecordSecondaryStream = false;
    option.keepArchivedMinForAuto = true;
    option.keepArchivedMinFor = 0;
    option.keepArchivedMaxForAuto = false;
    option.keepArchivedMaxFor = 28800;
    option.mediaPort = 0;
    option.autoMediaPort = false;
    option.rtpTransport = 0;
}

static void fromJson(unordered_map<int, StreamTuple> &ret, const Json::Value &data) {
    ret.clear();
    string project_id = data["project_id"].asString();
    string device_id = data["device_id"].asString();
    int index = 0;
    for (const auto &st : data["streams"]) {
        string stream_id = st["channel_id"].asString();
        string stream_url = st["source_url"].asString();
        StreamTuple tuple;
        tuple.vhost = DEFAULT_VHOST;
        tuple.device_id = device_id;
        tuple.stream_id = stream_id;
        tuple.name = getStreamTypeString(index);
        tuple.full_url = stream_url;
        ret[index++] = tuple;
    }
}

static void loadServerConfigFromJson(const Json::Value &data) {
    auto &ini = mINI::Instance();
    // failover config
    bool enableFailover = data["failover"].asBool();
    int maxNumberCamera = data["maxNumberCamera"].asInt();
    int serverLocationId = data["serverLocationId"].asInt();
    ini[Manager::kEnableFailover] = enableFailover;
    ini[Manager::kMaxAllowedDevices] = maxNumberCamera;
    ini[Manager::kServerLocationId] = serverLocationId;

    //todo: cấu hình lưu bookmark, cấu hình lưu video push, số lượng thiết bị tối đa cho phép

    // Reload config and save file 
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    ini.dumpFile(g_ini_file);

    // monitor threshold config
#define GET_THRESHOLD(type, name)                                                                                                                              \
    double levelLow_##type = !data["thresholdConfig"].isNull() ? data["thresholdConfig"][#name "_levelLow"].asDouble() : -1;                                   \
    double levelMedium_##type = !data["thresholdConfig"].isNull() ? data["thresholdConfig"][#name "_levelMedium"].asDouble() : -1;                             \
    GlobalMonitor::Instance().setThreshold(ResourceType::type, levelLow_##type, levelMedium_##type);
    GET_THRESHOLD(CPU, CPU);
    GET_THRESHOLD(MEMORY, RAM);
    GET_THRESHOLD(HDD, STORAGE);
}

static void loadServerClusterFromJson(const Json::Value &data) {
    //todo: cấu hình cluster
}

static Json::Value exampleJson() {
    Json::Value data;
    data["devices"] = Json::arrayValue;
    Json::Value device;
    device["device_id"] = "5abab589-88ec-450a-9096-e68fcbfa84fb";
    device["username"] = "admin";
    device["password"] = "Haiphong2025";
    device["manufacturer"] = "Hikivision";
    device["model"] = "DS-2CD2347G1-L";
    device["enable"] = true;
    device["address"] = "27.72.173.71";
    device["http_port"] = 80;
    device["is_enable"] = true;
    device["enable_recording"] = true;
    device["keep_archived_min_for_auto"] = true;
    device["keep_archived_min_for"] = 0;
    device["keep_archived_max_for_auto"] = false;
    device["keep_archived_max_for"] = 10 * 60;
    device["rtp_transport"] = 0;
    device["streams"] = Json::arrayValue;
    Json::Value channel_1;
    channel_1["channel_id"] = "0aa9322f-c0a3-4518-8273-8a7df3d35ede";
    // channel_1["source_url"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile2/media.smp";
    channel_1["source_url"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile5/media.smp";
    device["streams"].append(channel_1);
    // Json::Value channel_2;
    // channel_2["channel_id"] = "56c14e52-e578-40c3-8b50-d7c315a36456";
    // channel_2["source_url"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile4/media.smp";
    // device["streams"].append(channel_2);

    data["devices"].append(device);
    return data;
}

void loadServerConfigJson(const Json::Value &data) {
#if 0
    data = exampleJson();
#endif
    if (data.isMember("mediaServer")) {
        loadServerConfigFromJson(data["mediaServer"]);
    }

    if (data.isMember("list_media_server") && data["list_media_server"].isArray()) {
        loadServerClusterFromJson(data["list_media_server"]);
    }

    if (data.isMember("devices") && data["devices"].isArray()) {
        // get vector of current camera key 
        auto current_cameras = CameraManager::Instance().getCameraKeys();

        for (const auto &camera : data["devices"]) {
            // Get camera config from json data
            CameraInfo info;
            fromJson(info, camera);
            CameraOption option;
            fromJson(option, camera);
            unordered_map<int, StreamTuple> stream_map;
            fromJson(stream_map, camera);

            // Add or update camera config
            CameraManager::Instance().addCamera(info, option, stream_map);

            // Remove active camera key from vector
            current_cameras.erase(std::remove(current_cameras.begin(), current_cameras.end(), info.shortUrl()), current_cameras.end());
        }

        // Remove all inactive camera
        for (const auto &key : current_cameras) {
            CameraManager::Instance().delCamera(key);
        }
    }
}

static Json::Value makeMediaSourceJson(MediaSource &media) {
    Json::Value item;
    // auto media_tuple = media.getMediaTuple();
    // item["deviceId"] = media_tuple.app;
    // item["streamId"] = media_tuple.stream;
    // item["status"] = media.getTracks(true).size() > 0;
    // for (auto &track : media.getTracks(false)) {
    //     auto codec_type = track->getTrackType();
    //     if (codec_type == TrackAudio) {
    //         auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
    //         item["acodec"] = track->getCodecName();
    //         item["channels"] = audio_track->getAudioChannel();
    //         item["sample_rate"] = audio_track->getAudioSampleRate();
    //         item["sample_bit"] = audio_track->getAudioSampleBit();
    //     }
    //     if (codec_type == TrackVideo) {
    //         auto video_track = dynamic_pointer_cast<VideoTrack>(track);
    //         item["vcodec"] = track->getCodecName();
    //         item["width"] = video_track->getVideoWidth();
    //         item["height"] = video_track->getVideoHeight();
    //         item["bitrate"] = video_track->getBitRate();
    //         int gop_size = video_track->getVideoGopSize();
    //         int gop_interval_ms = video_track->getVideoGopInterval();
    //         float fps = video_track->getVideoFps();
    //         if (fps <= 1 && gop_interval_ms) {
    //             fps = gop_size * 1000.0 / gop_interval_ms;
    //         }
    //         item["fps"] = round(fps);
    //         item["gop_size"] = gop_size;
    //         item["gop_interval_ms"] = gop_interval_ms;
    //     }
    // }
    
    item["channelId"] = media.getMediaTuple().stream;
    item["status"] = media.getTracks(true).size() > 0 ? 1 : 0;
    for (auto &track : media.getTracks(false)) {
        auto codec_type = track->getTrackType();
        if (codec_type == TrackVideo) {
            auto video_track = dynamic_pointer_cast<VideoTrack>(track);
            item["codec"] = track->getCodecName();
            item["width"] = video_track->getVideoWidth();
            item["height"] = video_track->getVideoHeight();
        }
    }
    item["volumeSize"] = 0;
    item["volumeRate"] = 0;
    item["oldestTenMinutesBlock"] = 0;
    return item;
}

static Json::Value makeStreamStatisticJson(GenericRtspCameraImp::Ptr &camera, int type) {
    auto tuple = camera->getStreamTuple(type);
    auto src = MediaSource::find(tuple.vhost, tuple.device_id, tuple.stream_id);
    if (src) {
        return makeMediaSourceJson(*src);
    }
    Json::Value item;
    item["channelId"] = tuple.stream_id;
    // item["streamId"] = tuple.stream_id;
    item["status"] = 0;
    item["codec"] = "";
    item["width"] = 0;
    item["height"] = 0;
    item["volumeSize"] = 0;
    item["volumeRate"] = 0;
    item["oldestTenMinutesBlock"] = 0;
    return item;
}

void getServerStatisticJson(const function<void(Json::Value &data)> &cb) {
    Json::Value data = Json::arrayValue;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &device) {
        auto camera = dynamic_pointer_cast<GenericRtspCameraImp>(device);
        if (camera) {
            Json::Value item;
            auto tuple = camera->getCameraInfo();
            item["cameraId"] = tuple.device_id;
            item["channels"] = Json::arrayValue;
            if (camera->hasPrimaryStream()) {
                item["channels"].append(makeStreamStatisticJson(camera, PrimaryStream));
            }
            if (camera->hasSecondaryStream()) {
                item["channels"].append(makeStreamStatisticJson(camera, SecondaryStream));
            }
            data.append(item);
        }
    });
    cb(data);
}

void getServerUsageJson(const function<void(Json::Value &data)> &cb) {
    Json::Value data;
    int numCritical = 0;
    int numWarning = 0;
    int numNormal = 0;

#define COUNT_ALERT(usage_value, low_threshold, high_threshold)                                                                                                \
    if (high_threshold > 0 && usage_value >= high_threshold) {                                                                                                 \
        numCritical++;                                                                                                                                         \
    } else if (low_threshold > 0 && usage_value >= low_threshold) {                                                                                            \
        numWarning++;                                                                                                                                          \
    } else {                                                                                                                                                   \
        numNormal++;                                                                                                                                           \
    }
    auto cpu_usage = GlobalMonitor::Instance().getCpuUsage();
    auto cpu_threshold = GlobalMonitor::Instance().getThreshold(ResourceType::CPU);
    COUNT_ALERT(cpu_usage.usagePct, cpu_threshold.first, cpu_threshold.second)

    auto mem_usage = GlobalMonitor::Instance().getMemUsage();
    auto mem_threshold = GlobalMonitor::Instance().getThreshold(ResourceType::MEMORY);
    COUNT_ALERT(mem_usage.usagePct, mem_threshold.first, mem_threshold.second)

    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    auto hdd_threshold = GlobalMonitor::Instance().getThreshold(ResourceType::HDD);
    for (const auto& disk: hdd_usage) {
        COUNT_ALERT(disk.usage_pct, hdd_threshold.first, hdd_threshold.second)
    }
    
    size_t mainStorageUsedBytes = 0;
    size_t mainStorageTotalBytes = 0;
    StorageManager::Instance().getMainStorageUsage(mainStorageUsedBytes, mainStorageTotalBytes);

    data["cpuUsage"] = sanitize_for_json(cpu_usage.usagePct);
    data["ramUsage"] = sanitize_for_json(mem_usage.usagePct);
    data["currentStorageUsage"] = mainStorageUsedBytes;
    data["maxStorageCapacity"] = mainStorageTotalBytes;
    data["numCritical"] = numCritical;
    data["numWarning"] = numWarning;
    data["numNormal"] = numNormal;
    cb(data);
}

static Json::Value makeDeviceStorageJson(DeviceSource &device) {
    Json::Value data;
    // todo: 
    data["bytesSpeed"] = 0;
    data["oldestTimeBlock"] = 0;
    data["volumeSize"] = 0;
    return data;
}

void getStorageStatisticJson(const function<void(Json::Value &data)> &cb) {
    Json::Value data = Json::arrayValue;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &device) {
        data.append(makeDeviceStorageJson(*device));
    });
    cb(data);
}