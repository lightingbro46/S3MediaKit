#include <cmath>
#include <ctime>
#include <sstream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/base64.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Record/Recorder.h"
#include "Local/TimeRecorderManager.h"
#include "Local/TimeQuery.h"
#include "Storage/MigrationHistory.h"
#include "Local/StorageManager.h"
#include "Server/GlobalMonitor.h"
#include "Camera/CameraManager.h"
#include "Extension/Benchmark.h"
#include "Manager.h"
#include "Server/ClusterManager.h"
#include "Local/StatisticRecorder.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace managerkit {

namespace Manager {
#define MANAGER_FIELD "manager."
const string kMediaServerDomain = MANAGER_FIELD"mediaServerDomain";
const string kMediaServerProjectId = MANAGER_FIELD"mediaServerProjectId";
const string kMaxAllowedDevices = MANAGER_FIELD"maxAllowedDevices";
const string kMaxAvailableDevices = MANAGER_FIELD"maxAvailableDevices";
const string kServerLocationId = MANAGER_FIELD"serverLocationId";
const string kEnableFailover = MANAGER_FIELD"enableFailover";
const string kEnableAuthorize = MANAGER_FIELD"enableAuthorize";
const string kJwtPublicKey = MANAGER_FIELD"jwtPublicKey";
const string kSessionExpiryDays = MANAGER_FIELD"sessionExpiryDays";
const string kMaxStreamTimeoutSec = MANAGER_FIELD"maxStreamTimeoutSec";
const string kBypassAuthRealm = MANAGER_FIELD"bypassAuthRealm";

static onceToken token([]() {
    mINI::Instance()[kMediaServerDomain] = "";
    mINI::Instance()[kMediaServerProjectId] = "";
    mINI::Instance()[kMaxAllowedDevices] = 0;
    mINI::Instance()[kMaxAvailableDevices] = 256;
    mINI::Instance()[kServerLocationId] = 1;
    mINI::Instance()[kEnableFailover] = false;
    mINI::Instance()[kEnableAuthorize] = true;
    mINI::Instance()[kJwtPublicKey] = "";
    mINI::Instance()[kSessionExpiryDays] = 180;
    mINI::Instance()[kMaxStreamTimeoutSec] = 10.0;
    mINI::Instance()[kBypassAuthRealm] = "";
});
} // namespace Manager

} // namespace managerkit

static void enforceStoragePolicy() {
    DebugL << "Storage manager has been started monitoring";
    StorageManager::Instance().start();
}

static void loadSavedDeviceInfo() {
    EventPollerPool::Instance().getPoller()->doDelayTask(3000, []() {
        DebugL << "Camera manager has been started loading saved camera";
        CameraManager::Instance().loadSavedCameraInfo();
        return 0;
    });
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
        auto encoded_path = encodeBase64(info.file_path);
        block.set_file_path(encoded_path);

        auto ret = TimeRecorderManager::Instance().addBlock(block);
        if (ret) {
            StatisticRecorder::Instance().addArchiveSize(block.app(), block.stream(), 1, block.file_size(), block.start_time(), block.start_time() + block.time_len(), true);
        }
    });

    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastMediaSeeked, [](BroadcastMediaSeekedArgs) {
        auto infos = split(args.stream, "/");
        MediaTuple tuple = { args.vhost, infos[0], infos[1], "" };
        TimeQuery::Ptr query;
        int64_t offset = -1;
        try {
            query = make_shared<TimeQuery>(tuple);
        } catch(...) {}
        
        if (query) {
            // find data at this stamp
            bool found = false;
            query->getRecordedTimePeriod(stamp, stamp + 60, [&](vector<TimeRange> &ret) {
                for (auto const &p : ret) {
                    if (p.startTime == stamp) {
                        found = true;
                    }
                }
            });
            // find offset duration in date if this stamp has data
            if (found) {
                offset = query->getOffsetOfDate(stamp);
            }   
        }
        invoker(offset);
    });

    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastMediaSeeked2, [](BroadcastMediaSeeked2Args) {
        TimeQuery::Ptr query;
        uint64_t duration = 0;
        std::map<uint64_t, std::string> files;
        try {
            query = make_shared<TimeQuery>(args);
        } catch(...) {}
        
        if (query) {
            // find data at this stamp
            bool found = false;
            TimeRange first_range;
            query->getRecordedTimePeriod(stamp, stamp + max_duration, [&](vector<TimeRange> &ret) {
                for (auto const &p : ret) {
                    if (p.startTime == stamp) {
                        found = true;
                        first_range = p;
                    }
                }
            });
            // find offset duration in date if this stamp has data
            if (found) {
                query->getRecordedTimePeriod(first_range.startTime, first_range.startTime + first_range.duration, [&](vector<TimeBlock> &ret) {
                    for (const auto &block : ret) {
                        duration += block.time_len();
                        files.emplace(block.start_time(), decodeBase64(block.file_path()));
                    }
                });
            }
        }
        invoker(duration, files);
    });
#endif // ENABLE_MP4

    enforceStoragePolicy();

    loadSavedDeviceInfo();
}

static void releaseAllDevice() {
    // release all camera
    CameraManager::Instance().clear();
    // sleep for 3 second before uninstall hook, to prevent resource release order errors
    sleep(3);
}

void unInstallManagerHook() {
    releaseAllDevice();
    // Note: Comment the following code in order to save last segments when program exit
    NoticeCenter::Instance().delListener(&manager_hook_tag);
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
    // string name = data["name_device"].asString();
    string name = data["device_name"].asString();
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
    string record_scheduler = data["record_scheduler"].asString();
    bool do_not_record_primary_stream = data["do_not_record_primary_stream"].asBool();
    bool do_not_record_secondary_stream = data["do_not_record_secondary_stream"].asBool();
    bool keep_archived_min_for_auto = data["keep_archived_min_for_auto"].asBool();
    int keep_archived_min_for = data["keep_archived_min_for"].asInt();
    bool keep_archived_max_for_auto = data["keep_archived_max_for_auto"].asBool();
    int keep_archived_max_for = data["keep_archived_max_for"].asInt();
    int media_port = data["media_port"].asInt();
    bool media_port_auto = data["media_port_auto"].asBool();
    int rtp_transport = data["rtp_transport"].asInt();
    string prefered_media_server = data["pri_media_server"].asString();
    bool enable_ptz_control = data["enable_ptz_control"].asBool();

    // option.enableActive = enable_camera;
    // option.enableRecord = enable_recording;
    // option.recordScheduler = record_scheduler;
    // option.doNotRecordPrimaryStream = do_not_record_primary_stream;
    // option.doNotRecordSecondaryStream = do_not_record_secondary_stream;
    // option.keepArchivedMinForAuto = keep_archived_min_for_auto;
    // option.keepArchivedMinFor = keep_archived_min_for;
    // option.keepArchivedMaxForAuto = keep_archived_max_for_auto;
    // option.keepArchivedMaxFor = keep_archived_max_for;
    // option.mediaPort = media_port;
    // option.autoMediaPort = media_port_auto;
    // option.rtpTransport = rtp_transport;
    // GET_CONFIG(string, mediaServerId, General::kMediaServerId)
    // option.enableFailover = prefered_media_server != mediaServerId;
    // option.preferedMediaServer = prefered_media_server;
    // option.enablePTZControl = enable_ptz_control;
    bool enable_recording_ = false;
    uint64_t retention_ = 0;
    for (const auto &stream : data["streams"]) {
        if (stream["is_storing"].asBool()) {
            enable_recording_ = true;
        }
        uint64_t stream_retention = stream["retention_time"].isNull() ? 0 : static_cast<uint64_t>(stream["retention_time"].asFloat());
        auto retention = stream_retention * 3600;
        if (retention_ == 0 || retention < retention_) {
            retention_ = retention;
        }
    }
    option.enableActive = enable_camera;
    option.enableRecord = enable_recording_;
    option.recordScheduler = "";
    option.doNotRecordPrimaryStream = false;
    option.doNotRecordSecondaryStream = false;
    option.keepArchivedMinForAuto = true;
    option.keepArchivedMinFor = 0;
    option.keepArchivedMaxForAuto = false;
    option.keepArchivedMaxFor = retention_;
    option.mediaPort = 0;
    option.autoMediaPort = false;
    option.rtpTransport = 0;
    GET_CONFIG(string, mediaServerId, General::kMediaServerId)
    option.enableFailover = prefered_media_server != mediaServerId;
    option.preferedMediaServer = prefered_media_server;
    option.enablePTZControl = true;
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
    int change = 0;
    auto &ini = mINI::Instance();
    // failover config
    bool enableFailover = data["failover"].asBool();
    bool currentEnableFailover = ini[Manager::kEnableFailover];
    if (currentEnableFailover != enableFailover) {
        ini[Manager::kEnableFailover] = enableFailover;
        change++;
    }
    int maxNumberCamera = data["maxNumberCamera"].asInt();
    int currentMaxNumberCamera = ini[Manager::kMaxAllowedDevices];
    if (currentMaxNumberCamera != maxNumberCamera) {
        ini[Manager::kMaxAllowedDevices] = maxNumberCamera;
        change++;
    }
    int serverLocationId = data["serverLocationId"].asInt();
    int currentServerLocationId = ini[Manager::kServerLocationId];
    if (currentServerLocationId != serverLocationId) {
        ini[Manager::kServerLocationId] = serverLocationId;
        change++;
    }
    string mediaServerProjectId = !data["projectId"].isNull() ? data["projectId"].asString() : "";
    string currentMediaServerProjectId = ini[Manager::kMediaServerProjectId];
    if (currentMediaServerProjectId != mediaServerProjectId) {
        ini[Manager::kMediaServerProjectId] = mediaServerProjectId;
        change++;
    }

    //todo: cấu hình lưu bookmark, cấu hình lưu video push, số lượng thiết bị tối đa cho phép

    // Reload config and save file 
    if (change > 0) {
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
        ini.dumpFile(g_ini_file);
    }

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
    ClusterManager::Instance().clearAllMediaServer();
    for (const auto &server_info : data) {
        ClusterManager::Instance().addMediaServer(server_info);
    }
}

static Json::Value exampleJson() {
    Json::Value data;
    data["devices"] = Json::arrayValue;
    Json::Value device;
    device["device_id"] = "5abab589-88ec-450a-9096-e68fcbfa84fb";
    device["device_name"] = "Camera HPG";
    device["username"] = "admin";
    device["password"] = "Haiphong2025";
    device["manufacturer"] = "Hikivision";
    device["model"] = "DS-2CD2347G1-L";
    device["enable"] = true;
    device["address"] = "27.72.173.71";
    device["http_port"] = 8080;
    device["is_enable"] = true;
    device["enable_recording"] = true;
    device["enable_ptz_control"] = true;
    device["keep_archived_min_for_auto"] = true;
    device["keep_archived_min_for"] = 0;
    device["keep_archived_max_for_auto"] = false;
    device["keep_archived_max_for"] = 10 * 60;
    device["rtp_transport"] = 0;
    device["pri_media_server"] = mINI::Instance()[General::kMediaServerId];
    device["streams"] = Json::arrayValue;
    // Json::Value channel_1;
    // channel_1["channel_id"] = "0aa9322f-c0a3-4518-8273-8a7df3d35ede";
    // channel_1["source_url"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile1/media.smp";
    // device["streams"].append(channel_1);
    Json::Value channel_2;
    channel_2["channel_id"] = "56c14e52-e578-40c3-8b50-d7c315a36456";
    channel_2["source_url"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile5/media.smp";
    device["streams"].append(channel_2);

    data["devices"].append(device);
    return data;
}

void loadServerConfigJson(const Json::Value &data) {
    // auto data = exampleJson();
    TraceL << "Server configuration loaded: " << data.toStyledString();
    Ticker _ticker;

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
    DebugL << "Server configuration loaded completed, took " << _ticker.elapsedTime() << " ms";
}

static Json::Value makeMediaSourceJson(MediaSource &media) {
    Json::Value item;
    auto media_tuple = media.getMediaTuple();
    item["deviceId"] = media_tuple.app;
    item["streamId"] = media_tuple.stream;
    item["status"] = media.getTracks(true).size() > 0;
    for (auto &track : media.getTracks(false)) {
        auto codec_type = track->getTrackType();
        if (codec_type == TrackAudio) {
            auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
            item["acodec"] = track->getCodecName();
            item["channels"] = audio_track->getAudioChannel();
            item["sample_rate"] = audio_track->getAudioSampleRate();
            item["sample_bit"] = audio_track->getAudioSampleBit();
        }
        if (codec_type == TrackVideo) {
            auto video_track = dynamic_pointer_cast<VideoTrack>(track);
            item["vcodec"] = track->getCodecName();
            item["width"] = video_track->getVideoWidth();
            item["height"] = video_track->getVideoHeight();
            item["bitrate"] = video_track->getBitRate();
            int gop_size = video_track->getVideoGopSize();
            int gop_interval_ms = video_track->getVideoGopInterval();
            float fps = video_track->getVideoFps();
            if (fps <= 1 && gop_interval_ms) {
                fps = gop_size * 1000.0 / gop_interval_ms;
            }
            item["fps"] = round(fps);
            item["gop_size"] = gop_size;
            item["gop_interval_ms"] = gop_interval_ms;
        }
    }
    return item;
}

static Json::Value makeStreamStatisticJson(CameraStatistic &params, int type) {
    auto tuple = params.stream_map[type];
    auto info = params.sinfo_map[type];
    Json::Value item;
    // todo: change new format with more information
    item["channelId"] = tuple.stream_id;
    item["status"] = info.live ? 1 : 0;
    item["codec"] = info.vcodec;
    item["width"] = info.width;
    item["height"] = info.height;
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
            auto stats_imp = camera->getCameraStatisticImp();
            if (stats_imp) {
                auto params = stats_imp->getParams();
                auto option = params.option;
                if (option.enableFailover && !camera->isEnabled()) {
                    // this camera run in failover mode and actual camera connection run on prefered media server
                    return;
                }
                Json::Value item;
                item["cameraId"] = params.info.device_id;
                item["isPtz"] = params.device_caps.ptzCapabilities;
                item["channels"] = Json::arrayValue;
                if (camera->hasStreamTuple(PrimaryStream)) {
                    item["channels"].append(makeStreamStatisticJson(params, PrimaryStream));
                }
                if (camera->hasStreamTuple(SecondaryStream)) {
                    item["channels"].append(makeStreamStatisticJson(params, SecondaryStream));
                }
                data.append(item);
            }
        }
    });
    TraceL << "Server statistic report: " << data.toStyledString();
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

struct ServerStorageStatistic {
    int totalMainDevice = 0;
    uint64_t totalMainBitrate = 0;
    uint64_t totalMainUsedStorage = 0;
    int totalFailoverDevice = 0;
    uint64_t totalFailoverBitrate = 0;
    uint64_t totalFailoverUsedStorage = 0;
};

static Json::Value makeDeviceStorageJson(const DeviceSource::Ptr& device, ServerStorageStatistic &server_stats) {
    Json::Value ret;
    auto ptr = std::dynamic_pointer_cast<GenericRtspCameraImp>(device);
    if (ptr) {
        auto stats_imp = ptr->getCameraStatisticImp();
        if (stats_imp) {
            auto stats = stats_imp->getParams();
            ret["id"] = stats.info.device_id;
            ret["name"] = stats.info.name;
            int bytes_speed = 0;
            int desired_bytes_speed = 0;
            for (const auto &it : stats.sinfo_map) {
                if (it.second.live) {
                    bytes_speed += it.second.byte_speed;
                }
                desired_bytes_speed += it.second.byte_speed;
            }
            ret["bitrate"] = bytes_speed;
            ret["desiredBitrate"] = desired_bytes_speed;
            uint64_t oldest_time_block = 0;
            uint64_t used_storage = 0;
            for (const auto &it : stats.storage_map) {
                if (oldest_time_block == 0 || (it.second.archiveStartTime != 0 && it.second.archiveStartTime < oldest_time_block)) {
                    oldest_time_block = it.second.archiveStartTime;
                }
                used_storage += it.second.archiveSizeB;
            }
            ret["oldestTimeBlock"] = oldest_time_block;
            uint64_t desired_time_block = oldest_time_block;
            if (!stats.option.keepArchivedMaxForAuto) {
                desired_time_block = time(nullptr) - stats.option.keepArchivedMaxFor;
            }
            ret["desiredTimeBlock"] = desired_time_block;
            ret["usedStorage"] = used_storage;
            auto isFailover = stats.option.enableFailover;
            if (!isFailover) {
                server_stats.totalMainDevice++;
                server_stats.totalMainBitrate += bytes_speed;
                server_stats.totalMainUsedStorage += used_storage;
            } else {
                server_stats.totalFailoverDevice++;
                server_stats.totalFailoverBitrate += bytes_speed;
                server_stats.totalFailoverUsedStorage += used_storage;
            }
            ret["isFailover"] = isFailover;
        }
    }

    return ret;
}

Json::Value makeStorageStatisticJson() {
    Json::Value data ;
    data["mainDevices"] = Json::arrayValue;
    data["failoverDevices"] = Json::arrayValue;
    ServerStorageStatistic server_stats;

    DeviceSource::for_each_device([&](const DeviceSource::Ptr &device) {
        auto storage_json = makeDeviceStorageJson(device, server_stats);
        if (!storage_json.isNull()) {
            if (storage_json["isFailover"].asBool()) {
                data["failoverDevices"].append(storage_json);
            } else {
                data["mainDevices"].append(storage_json);
            }
        }
    }, CAMERA_SCHEMA);

    data["totalMainDevice"] = server_stats.totalMainDevice;
    data["totalMainBitrate"] = server_stats.totalMainBitrate;
    data["totalMainUsedStorage"] = server_stats.totalMainUsedStorage;
    data["totalFailoverDevice"] = server_stats.totalFailoverDevice;
    data["totalFailoverBitrate"] = server_stats.totalFailoverBitrate;
    data["totalFailoverUsedStorage"] = server_stats.totalFailoverUsedStorage;
    return data;
}

static Json::Value makeStreamStatisticJson2(CameraStatistic &params, int type) {
    Json::Value ret;
    auto tuple = params.stream_map[type];
    auto info = params.sinfo_map[type];
    Json::Value item;
    ret["id"] = tuple.stream_id;
    ret["live"] = info.live;
    ret["status"] = info.status;
    ret["vcodec"] = info.vcodec;
    ret["width"] = info.width;
    ret["height"] = info.height;
    ret["bitrate"] = info.bitrate;
    ret["fps"] = info.fps;
    ret["byteSpeed"] = info.byte_speed;
    ret["acodec"] = info.acodec;
    ret["sample_rate"] = info.sample_rate;
    ret["channel_no"] = info.channel_no;
    ret["sample_bit"] = info.sample_bit;
    return ret;
}

static Json::Value makeDeviceStatisticJson(const DeviceSource::Ptr& device) {
    Json::Value ret;
    auto ptr = std::dynamic_pointer_cast<GenericRtspCameraImp>(device);
    if (ptr) {
        auto stats_imp = ptr->getCameraStatisticImp();
        if (stats_imp) {
            auto stats = stats_imp->getParams();
            ret["id"] = stats.info.device_id;
            ret["name"] = stats.info.name;
            ret["controller"]["connect"] = stats.device_caps.connect;
            ret["controller"]["status"] = stats.device_caps.status;
            ret["controller"]["ptz"] = stats.device_caps.ptzCapabilities;
            ret["streams"] = Json::arrayValue;
            if (ptr->hasStreamTuple(PrimaryStream)) {
                ret["streams"].append(makeStreamStatisticJson2(stats, PrimaryStream));
            }
            if (ptr->hasStreamTuple(SecondaryStream)) {
                ret["streams"].append(makeStreamStatisticJson2(stats, SecondaryStream));
            }
        }
    }
    return ret;
}

Json::Value makeAllDeviceStatisticJson() {
    Json::Value data = Json::arrayValue;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &device) {
        auto device_json = makeDeviceStatisticJson(device);
        if (!device_json.isNull()) {
            data.append(device_json);
        }
    }, CAMERA_SCHEMA);
    return data;
}

void loadServerStartedConfigJson(const Json::Value &data) {
    int change = 0;
    auto &ini = mINI::Instance();
    // public key
    if (data.isMember("publicKey")) {
        string publicKey = data["publicKey"].asString();
        auto base64_publicKey = encodeBase64(publicKey);
        if (ini[Manager::kJwtPublicKey] != base64_publicKey) {
            ini[Manager::kJwtPublicKey] = base64_publicKey;
            change++;
        }
    }

    // save ini file if there are changes
    if (change > 0) {
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
        ini.dumpFile(g_ini_file);
    }
}

Json::Value makeSystemStatisticJson() {
    Json::Value val;
    auto osinfo = GlobalMonitor::Instance().getOsInfo();
    val["osInfo"]["platform"] = osinfo.platform;
    val["osInfo"]["variant"] = osinfo.variant;
    val["osInfo"]["variant_verison"] = osinfo.variant_version;

    auto cpu_usage = GlobalMonitor::Instance().getCpuUsage();
    val["cpu"]["cores"] = cpu_usage.cores;
    val["cpu"]["usage_pct"] = sanitize_for_json(cpu_usage.usagePct);
    val["cpu"]["proc_usage_pct"] = sanitize_for_json(cpu_usage.procUsagePct);

    auto mem_usage = GlobalMonitor::Instance().getMemUsage();
    val["ram"]["used"] = (Json::UInt64)mem_usage.usageMemory;
    val["ram"]["total"] = (Json::UInt64)mem_usage.totalMemory;
    val["ram"]["usage_pct"] = sanitize_for_json(mem_usage.usagePct);
    val["ram"]["proc_usage_pct"] = sanitize_for_json(mem_usage.procUsagePct);

    val["nets"] = Json::arrayValue;
    auto net_usage = GlobalMonitor::Instance().getNetUsage();
    for (const auto &n : net_usage) {
        Json::Value net_val;
        net_val["name"] = n.name;
        net_val["ipv4"] = n.ipv4;
        net_val["ipv6"] = n.ipv6;
        net_val["mac"] = n.mac_address;
        net_val["rx_mbps"] = sanitize_for_json(n.rx_mbps);
        net_val["tx_mbps"] = sanitize_for_json(n.tx_mbps);
        net_val["speed_mbps"] = n.speed_mbps;
        val["nets"].append(net_val);
    }
   
    val["disks"] = Json::arrayValue;
    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    for (const auto &d : hdd_usage) {
        Json::Value disk;
        disk["name"] = d.device;
        disk["mount"] = d.mount_point;
        disk["used"] = (Json::UInt64)d.used_bytes;
        disk["total"] = (Json::UInt64)d.total_bytes;
        disk["used_pct"] = sanitize_for_json(d.usage_pct);
        val["disks"].append(disk);
    }

    auto cpu_threshold = GlobalMonitor::Instance().getThreshold(ResourceType::CPU);
    val["threshold"]["cpu_levelLow"] = cpu_threshold.first;
    val["threshold"]["cpu_levelMedium"] = cpu_threshold.second;
    auto mem_threshold = GlobalMonitor::Instance().getThreshold(ResourceType::MEMORY);
    val["threshold"]["ram_levelLow"] = mem_threshold.first;
    val["threshold"]["ram_levelMedium"] = mem_threshold.second;
    auto hdd_threshold = GlobalMonitor::Instance().getThreshold(ResourceType::HDD);
    val["threshold"]["disk_levelLow"] = hdd_threshold.first;
    val["threshold"]["disk_levelMedium"] = hdd_threshold.second;
    
    return val;
}

Json::Value makeSystemStorageJson() {
    Json::Value val = Json::arrayValue;
    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    auto main_mount_point = StorageManager::Instance().getMainStorageMountPoint();
    for (const auto &d : hdd_usage) {
        Json::Value disk;
        disk["name"] = d.device;
        disk["mount"] = d.mount_point;
        disk["used"] = (Json::UInt64)d.used_bytes;
        disk["total"] = (Json::UInt64)d.total_bytes;
        disk["isMainStorage"] = main_mount_point == d.mount_point;
        disk["enableConfigure"] = false;
        val.append(disk);
    }
    return val;
}

int estimateMaxAvailableDevice() {
    auto &ini = mINI::Instance();
    int maxAvailableDevice = ini[Manager::kMaxAvailableDevices];
    if (maxAvailableDevice != 0) {
        return maxAvailableDevice;
    }
    auto cpu_usage = GlobalMonitor::Instance().getCpuUsage();
    int cpu_core = cpu_usage.cores;
    auto mem_usage = GlobalMonitor::Instance().getMemUsage();
    uint64_t mem_cap = mem_usage.totalMemory / 1024 / 1024; // byte -> Megabyte
    auto net_usage = GlobalMonitor::Instance().getNetUsage();
    uint64_t net_cap = 0;
    for (const auto &net : net_usage) {
        if (net_cap == 0 || net_cap < net.speed_mbps) {
            net_cap = net.speed_mbps;
        }
    }
    uint64_t disk_cap = 200;
    auto hdd_usage = GlobalMonitor::Instance().getHddUsage();
    // estimate max available camera that can be run on server hardware
    maxAvailableDevice = Benchmark::estimateAvailableDevice(net_cap, disk_cap, cpu_core, mem_cap);
    // save param to file
    ini[Manager::kMaxAvailableDevices] = maxAvailableDevice;
    ini.dumpFile(g_ini_file);

    return maxAvailableDevice;
}

void countDeviceStatusJson(const Json::Value &data, int &online, int &offline) {
    online = 0;
    offline = 0;
    for (const auto &device : data) {
        bool is_online = false;
        for (const auto &stream : device["channels"]) {
            if (stream["status"].asInt() == 1) {
                is_online = true;
            }
        }
        if (is_online) {
            online++;
        } else {
            offline++;
        }
    }
}

void installGlobalMonitor() {
    // Start monitoring system resource usage
    GlobalMonitor::Instance().start();
}