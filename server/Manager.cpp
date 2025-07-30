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
#include "Common/CameraSource.h"
#include "Local/StorageManager.h"
#include "Server/GlobalMonitor.h"
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
    mINI::Instance()[kEnableAuthorize] = true;
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

// static CameraInfo fromJson(Json::Value &data) {
//     string camera_id = data["device_id"].asString();
//     string camera_name = data["name_device"].asString();
//     string manufacturer = data["manufacturer"].asString();
//     string model = data["model"].asString();
    
//     CameraTuple tuple;
//     tuple.camera_id = camera_id;
//     tuple.camera_name = camera_name;
//     tuple.manufacturer = manufacturer;
//     tuple.model = model;
//     return tuple;
// }

// static CameraCredentials fromJson(Json::Value &data) {
//     string username = data["username"].asString();
//     string password = data["password"].asString();
//     string ip = data["address"].asString();
//     int port = data["http_port"].asInt();
    
//     CameraCredentials credential;
//     credential.username = username;
//     credential.password = password;
//     credential.ip = ip;
//     credential.port = port
//     return credential;
// }

// static CameraOptions fromJson(Json::Value &data) {
//     bool enable_camera = data["is_enable"].asBool();
//     bool enable_recording = data["enable_recording"].asBool();
//     bool do_not_record_primary_stream = data["do_not_record_primary_stream"].asBool();
//     bool do_not_record_secondary_stream = data["do_not_record_secondary_stream"].asBool();
    
//     CameraOptions options;
//     options.enable_camera = enable_camera;
//     options.enable_recording = enable_recording;
//     options.do_not_record_primary_stream = do_not_record_primary_stream;
//     options.do_not_record_secondary_stream = do_not_record_secondary_stream;
//     return options;
// }

void loadServerConfigJson(const Json::Value &data) {
    if (data.isMember("mediaServer")) {
        auto &ini = mINI::Instance();
        auto mserver_data = data["mediaServer"];
        // failover config
        bool enableFailover = mserver_data["failover"].asBool();
        int maxNumberCamera = mserver_data["maxNumberCamera"].asInt();
        int serverLocationId = mserver_data["serverLocationId"].asInt();

        // monitor threshold config
#define GET_THRESHOLD(type, name)                                                                                                                              \
    double levelLow_##type = !mserver_data["thresholdConfig"].isNull() ? mserver_data["thresholdConfig"][#name "_levelLow"].asDouble() : -1;                   \
    double levelMedium_##type = !mserver_data["thresholdConfig"].isNull() ? mserver_data["thresholdConfig"][#name "_levelMedium"].asDouble() : -1;             \
    GlobalMonitor::Instance().setThreshold(ResourceType::type, levelLow_##type, levelMedium_##type);
        GET_THRESHOLD(CPU, CPU);
        GET_THRESHOLD(MEMORY, RAM);
        GET_THRESHOLD(HDD, STORAGE);

        //todo: cấu hình lưu bookmark, cấu hình lưu video push, số lượng thiết bị tối đa cho phép
    }

    if (data.isMember("list_media_server") && data["list_media_server"].isArray()) {
        //todo: cấu hình cluster
    }

    if (data.isMember("devices") && data["devices"].isArray()) {
        for (const auto &camera : data["cameras"]) {
            // camera tuple
            string camera_id = camera["device_id"].asString();
            string camera_name = camera["name_device"].asString();
            string manufacturer = camera["manufacturer"].asString();
            string model = camera["model"].asString();
            //onvif credentials
            string username = camera["username"].asString();
            string password = camera["password"].asString();
            string ip = camera["address"].asString();
            int port = camera["http_port"].asInt();
            // stream info
            string primary_url;
            string primary_id;
            string secondary_url;
            string secondary_id;
            if (camera.isMember("streams") && camera["streams"].isArray()) {
                if (1 <= camera["streams"].size()) {
                    Json::Value stream = camera["streams"][0];
                    primary_url = stream["source_url"].asString();
                    primary_id = stream["channel_id"].asString();
                }
                if (2 <= camera["streams"].size()) {
                    Json::Value stream = camera["streams"][1];
                    secondary_url = stream["source_url"].asString();
                    secondary_id = stream["channel_id"].asString();
                }
            }
            // camera options
            bool enable_camera = camera["is_enable"].asBool();
            bool enable_recording = camera["enable_recording"].asBool();
            bool do_not_record_primary_stream = camera["do_not_record_primary_stream"].asBool();
            bool do_not_record_secondary_stream = camera["do_not_record_secondary_stream"].asBool();

            // auto ret = CameraSource::find()
        }
    }

}

void getServerStatisticJson(const function<void(Json::Value &data)> &cb) {
    Json::Value data;
    // CameraSource::for_each_camera([&](const CameraSource::Ptr &camera) {

    // });
    cb(data);
}

void getServerUsageJson(const function<void(Json::Value &data)> &cb) {
    Json::Value data;
    int numCritical = 0;
    int numWarning = 0;
    int numNormal = 0;
    auto cpu_usage = GlobalMonitor::Instance().getCpuUsage();
    data["cpuUsage"] = cpu_usage.usagePct;
    auto mem_usage = GlobalMonitor::Instance().getMemUsage();
    data["ramUsage"] = mem_usage.usagePct;
    double mainStorageUsage = 0.0;
    size_t mainStorageTotalBytes = 0;
    StorageManager::Instance().getMainStorageUsage(mainStorageUsage, mainStorageTotalBytes);
    data["currentStorageUsage"] = mainStorageUsage;
    data["maxStorageCapacity"] = mainStorageTotalBytes;
    cb(data);
}