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
#include "Extension/Benchmark.h"
#include "Manager.h"
#include "Server/ClusterManager.h"
#include "Local/StatisticRecorder.h"
#include "Common/StrUtil.h"
#include "User/UserAuditLog.h"
#include "User/UserSessionCache.h"

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
        DebugL << "Record mp4 file " << info.app << "/" << info.stream << "/" << info.start_time << "/" << info.time_len << "/" << info.file_path;
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
                    if (p.startTime <= stamp + 1) {
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
                    // Note: We allow 1 second of gap to find the nearest block, since the timestamp may not be exactly the same as the start time of a block due to various reasons (e.g., recording delay, file writing delay, etc.)
                    if (p.startTime <= stamp + 1) {
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

    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastPlayerCountChanged, [](BroadcastPlayerCountChangedArgs) {
        auto device_id = args.app;
        bool record_stream = false;
        GET_CONFIG(string, app_name, Record::kAppName);
        if (args.app == app_name) {
            device_id = split(args.stream, "/")[0];
            record_stream = true;
        }
        GlobalMonitor::Instance().setStreamReaderCount(device_id, count, record_stream);
    });

    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastRecordMotion, [](BroadcastRecordMotionArgs) {
        DebugL << "Record motion event " << args.app << "/" << args.stream << "/" << bActive;
        auto device = DeviceSource::find(args.vhost, args.app); 
        if (!device) {
            WarnL << "Motion event from unknown device:" << args.vhost << "/" << args.app << ": " << bActive;
            return;
        }
        auto weak_listener = device->getListener();
        if (auto strong_listener = weak_listener.lock()) {
            auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
            if (impl) {
                auto poller = impl->getOwnerPoller(*device);
                if (poller) {
                    poller->async([impl, bActive](){
                        impl->setupRecordEvent(RecordEventType::Motion, bActive);
                    });
                }
            }
        }
    });

    // Listen to rtsp, rtmp source registration or deregistration events
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastMediaChanged, [](BroadcastMediaChangedArgs) {
        if (sender.getSchema() == RTSP_SCHEMA) {
            auto media_tuple = sender.getMediaTuple();
            auto device = DeviceSource::find(media_tuple.vhost, media_tuple.app);
            if (!device) {
                return;
            }
            auto camera = dynamic_pointer_cast<GenericRtspCamera>(device);
            if (!camera) {
                return;
            }
            int type = -1;
            if (camera->hasStreamTuple(StreamType::PrimaryStream)) {
                auto &tuple = camera->getStreamTuple(StreamType::PrimaryStream);
                if (tuple.stream_id == media_tuple.stream) {
                    type = StreamType::PrimaryStream;
                }
            } 
            if (camera->hasStreamTuple(StreamType::SecondaryStream)) {
                auto &tuple = camera->getStreamTuple(StreamType::SecondaryStream);
                if (tuple.stream_id == media_tuple.stream) {
                    type = StreamType::SecondaryStream;
                }
            } 
            if (type == -1) {
                return;
            }

            auto weak_listener = device->getListener();
            if (auto strong_listener = weak_listener.lock()) {
                auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                if (impl) {
                    auto poller = impl->getOwnerPoller(*device);
                    auto regist = bRegist;
                    if (poller) {
                        poller->async([impl, type, regist](){
                            impl->setupStreamRegist(type, regist);
                        });
                    }
                }
            }
        }
    });

    // Listen to system audit log events
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastSystemAuditLog, [](BroadcastSystemAuditLogArgs) { 
        // todo: save to database, currently we just print the log
        DebugL << "System audit log: " << event;
    });

    // Listen to user audit log events
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastUserAuditLog, [](BroadcastUserAuditLogArgs) {
        // todo: save to database, currently we just print the log
        DebugL << "User audit log: " << event;
    });

    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastMediaPublish, [](BroadcastMediaPublishArgs) {
        // todo: rtsp, rtmp video publish event, currently we only have stream pull event, we can add publish event later if needed
    });

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

#define GET_OPTION_PROPERTY(dst, name, src, key)                                                                                                               \
    if (!src[#key].isNull()) {                                                                                                                                 \
        dst.name = src[#key].as<decltype(dst.name)>();                                                                                                         \
    }

#define GET_OPTION_PROPERTY_AS_STRING(dst, name, src, key)                                                                                                     \
    if (!src[#key].isNull()) {                                                                                                                                 \
        dst.name = StrJsonUtils::writeJsonString(src[#key]);                                                                                                   \
    }

#define GET_OPTION_PROPERTY_OR_DEFAULT_VALUE(dst, name, src, key, default_value)                                                                               \
    if (!src[#key].isNull()) {                                                                                                                                 \
        dst.name = src[#key].as<decltype(dst.name)>();                                                                                                         \
    } else {                                                                                                                                                   \
        dst.name = default_value;                                                                                                                              \
    }

static void fromJson(DeviceTuple &tuple, const Json::Value &data) {
    // default vhost is __defaultVhost__
    tuple.vhost = DEFAULT_VHOST;

    GET_OPTION_PROPERTY(tuple, device_id, data, id)
    GET_OPTION_PROPERTY(tuple, name, data, name)
}

static void fromJson(CameraOption &option, const Json::Value &data) {
    GET_OPTION_PROPERTY(option, name, data, name)
    GET_OPTION_PROPERTY(option, manufacturer, data, manufacturer)
    GET_OPTION_PROPERTY(option, model, data, model)
    GET_OPTION_PROPERTY(option, username, data, username)
    GET_OPTION_PROPERTY(option, password, data, password)
    GET_OPTION_PROPERTY(option, ip, data, ip)
    // http port default 80
    GET_OPTION_PROPERTY_OR_DEFAULT_VALUE(option, port, data, httpPort, 80)
    GET_OPTION_PROPERTY(option, enableActive, data, enabled)
    GET_OPTION_PROPERTY(option, preferedMediaServer, data, priMediaServerId)
    GET_CONFIG(string, mediaServerId, General::kMediaServerId)
    option.enableFailover = option.preferedMediaServer != mediaServerId;
    GET_OPTION_PROPERTY(option, enablePTZControl, data, enablePtzControl)

    if (data.isMember("recordingConfig") && !data["recordingConfig"].isNull()) {
        const Json::Value &rc = data["recordingConfig"];

        GET_OPTION_PROPERTY(option, enableRecord, rc, enableRecording)
        GET_OPTION_PROPERTY_AS_STRING(option, recordSchedules, rc, recordingSchedule)
        GET_OPTION_PROPERTY(option, keepArchivedMinForAuto, rc, keepArchivedMinForAuto)
        GET_OPTION_PROPERTY_OR_DEFAULT_VALUE(option, keepArchivedMinFor, rc, keepArchivedMinFor, 0)
        // convert hour to second
        option.keepArchivedMinFor = option.keepArchivedMinFor * 3600;
        GET_OPTION_PROPERTY(option, keepArchivedMaxForAuto, rc, keepArchivedMaxForAuto)
        GET_OPTION_PROPERTY_OR_DEFAULT_VALUE(option, keepArchivedMaxFor, rc, keepArchivedMaxFor, 0)
        // convert hour to second
        option.keepArchivedMaxFor = option.keepArchivedMaxFor * 3600;

        GET_OPTION_PROPERTY(option, motionPreRecordSec, rc, motionPreRecordSec)
        GET_OPTION_PROPERTY(option, motionPostRecordSec, rc, motionPostRecordSec)
    }

    if (data.isMember("cameraAdvanceConfig") && !data["cameraAdvanceConfig"].isNull()) {
        const Json::Value &adv = data["cameraAdvanceConfig"];
        
        if (adv.isMember("streamSettings") && !adv["streamSettings"].isNull()) {
            const Json::Value &ss = adv["streamSettings"];

            GET_OPTION_PROPERTY(option, disablePrimaryStream, ss, disableMainStream)
            GET_OPTION_PROPERTY(option, disableSecondaryStream, ss, disableSubStream)
            GET_OPTION_PROPERTY(option, doNotRecordPrimaryStream, ss, notRecordMainStream)
            GET_OPTION_PROPERTY(option, doNotRecordSecondaryStream, ss, notRecordSubStream)
            GET_OPTION_PROPERTY(option, disableAudio, ss, disableAudio)
            GET_OPTION_PROPERTY(option, keepConfigProfileAndStream, ss, keepConfigProfileAndStream)
        }

        if (adv.isMember("onvif") && !adv["onvif"].isNull()) {
            const Json::Value &onvif = adv["onvif"];
            auto parseOnvifProfile = [](const Json::Value &v) {
                if (v.isNull() || !v.isString() || v.asString() == "AUTO")
                    return std::string("");
                return v.asString();
            };
            option.onvifMainProfile = parseOnvifProfile(onvif["mainStream"]);
            option.onvifSubProfile = parseOnvifProfile(onvif["subStream"]);
        }

        if (adv.isMember("mediaStreaming") && !adv["mediaStreaming"].isNull()) {
            const Json::Value &ms = adv["mediaStreaming"];

            GET_OPTION_PROPERTY(option, mediaPort, ms, mediaPort)
            GET_OPTION_PROPERTY(option, autoMediaPort, ms, useDefaultMediaPort)

            auto parseRtpTransport = [](const Json::Value &v) {
                if (v.isNull() || !v.isString())
                    return CameraOption::kRtpTransportAuto;
                if (v.asString() == "TCP")
                    return CameraOption::kRtpTransportTcp;
                if (v.asString() == "UDP")
                    return CameraOption::kRtpTransportUdp;
                if (v.asString() == "MULTI")
                    return CameraOption::kRtpTransportMultiCast;
                return CameraOption::kRtpTransportAuto;
            };
            option.rtpTransport = parseRtpTransport(ms["rtpTransport"]);
        }

        if (adv.isMember("webPage") && !adv["webPage"].isNull()) {
            const Json::Value &wp = adv["webPage"];

            GET_OPTION_PROPERTY(option, webPort, wp, webPort)
            GET_OPTION_PROPERTY(option, autoWebPort, wp, useDefaultWebPort)
        }

        if (adv.isMember("ptzSetting") && !adv["ptzSetting"].isNull()) {
            const Json::Value &ptz = adv["ptzSetting"];

            GET_OPTION_PROPERTY(option, enablePTZControl, ptz, enablePTZControl)
            GET_OPTION_PROPERTY(option, reversePanAxis, ptz, reversePanAxis)
            GET_OPTION_PROPERTY(option, reverseTiltAxis, ptz, reverseTiltAxis)

            auto parsePTZMode = [](const Json::Value &v) {
                if (v.isNull() || !v.isString())
                    return CameraOption::kPTZModeAuto;
                if (v.asString() == "ABSOLUTE")
                    return CameraOption::kPTZAbsolutedMode;
                if (v.asString() == "RELATIVE")
                    return CameraOption::kPTZRelativeMode;
                if (v.asString() == "CONTINUOUS")
                    return CameraOption::kPTZContinousMode;
                if (v.asString() == "AUTO")
                    return CameraOption::kPTZModeAuto;
                return CameraOption::kPTZModeAuto;
            };
            option.ptzMode = parsePTZMode(ptz["ptzModeSelected"]);
            GET_OPTION_PROPERTY(option, ptzSpeed, ptz, ptzSpeed)
        }
    }

    if (data.isMember("motionDetectConfig") && !data["motionDetectConfig"].isNull()) {
        const Json::Value &mdc = data["motionDetectConfig"];

        GET_OPTION_PROPERTY(option, enableMotion, mdc, clientEnabled)
        GET_OPTION_PROPERTY(option, roiValue, mdc, value)

        auto parseStreamType = [](const Json::Value &v) {
            if (v.isNull() || !v.isString())
                return StreamType::SecondaryStream;
            if (v.asString() == "PRIMARY")
                return StreamType::PrimaryStream;
            if (v.asString() == "SECONDARY")
                return StreamType::SecondaryStream;
            return StreamType::SecondaryStream;
        };
        option.motionDetectOnStream = parseStreamType(mdc["chooseStream"]);
    }
}

static void fromJson(unordered_map<int, StreamTuple> &ret, const Json::Value &data) {
    ret.clear();
    string device_id = data["id"].asString();
    if (!data["primaryStreamId"].isNull() && !data["primaryStreamUrl"].isNull()) {
        auto stream_id = data["primaryStreamId"].asString();
        auto stream_url = data["primaryStreamUrl"].asString();
        if (!stream_id.empty() && !stream_url.empty()) {
            StreamTuple tuple;
            // default vhost is __defaultVhost__
            tuple.vhost = DEFAULT_VHOST;
            tuple.device_id = device_id;
            tuple.stream_id = stream_id;
            tuple.name = getStreamTypeString(PrimaryStream);
            tuple.full_url = stream_url;
            ret[PrimaryStream] = tuple;
        }
    }
    if (!data["secondaryStreamId"].isNull() && !data["secondaryStreamUrl"].isNull()) {
        auto stream_id = data["secondaryStreamId"].asString();
        auto stream_url = data["secondaryStreamUrl"].asString();
        if (!stream_id.empty() && !stream_url.empty()) {
            StreamTuple tuple;
            // default vhost is __defaultVhost__
            tuple.vhost = DEFAULT_VHOST;
            tuple.device_id = device_id;
            tuple.stream_id = stream_id;
            tuple.name = getStreamTypeString(SecondaryStream);
            tuple.full_url = stream_url;
            ret[SecondaryStream] = tuple;
            return;
        }
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
    int maxNumberCamera = data["maxConfigCameras"].asInt();
    int currentMaxNumberCamera = ini[Manager::kMaxAllowedDevices];
    if (currentMaxNumberCamera != maxNumberCamera) {
        ini[Manager::kMaxAllowedDevices] = maxNumberCamera;
        change++;
    }
    int serverLocationId = data["serverGroupId"].asInt();
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

    //todo: cấu hình lưu video push

    // Reload config and save file 
    if (change > 0) {
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
        ini.dumpFile(g_ini_file);
    }

    // stream reader threshold config
    bool unlimitedConnectPerCamera = !data["unlimitedStreamPerCamera"].isNull() ? data["unlimitedStreamPerCamera"].asBool() : false;
    int maxConnectPerCamera = !data["maxStreamPerCamera"].isNull() ? data["maxStreamPerCamera"].asInt() : -1;
    int maxConnectPerCameraByLicense = !data["streamMaxCameraOfLicense"].isNull() ? data["streamMaxCameraOfLicense"].asInt() : -1;
    if (!unlimitedConnectPerCamera && maxConnectPerCamera > 0 && maxConnectPerCameraByLicense > 0) {
        auto value = min(maxConnectPerCamera, maxConnectPerCameraByLicense);
        GlobalMonitor::Instance().setStreamReaderThreshold(value, (int)(value * 1.1));
    } else if (unlimitedConnectPerCamera && maxConnectPerCameraByLicense > 0) {
        GlobalMonitor::Instance().setStreamReaderThreshold(maxConnectPerCameraByLicense, (int)(maxConnectPerCameraByLicense * 1.1));
    } else {
        GlobalMonitor::Instance().setStreamReaderThreshold(-1, -1);
    }

    bool unlimitedConnectOnMserver = !data["unlimitedStream"].isNull() ? data["unlimitedStream"].asBool() : false;
    int maxConnectOnMserver = !data["maxStream"].isNull() ? data["maxStream"].asInt() : -1;
    int maxConnectOnMserverByLicense = !data["streamMaxOfLicense"].isNull() ? data["streamMaxOfLicense"].asInt() : -1;
    if (!unlimitedConnectOnMserver && maxConnectOnMserver > 0  && maxConnectOnMserverByLicense > 0) {
        auto value = min(maxConnectPerCamera, maxConnectPerCameraByLicense);
        GlobalMonitor::Instance().setThreshold(ResourceType::READER, value, (int)(value * 1.1));
    } else if (unlimitedConnectOnMserver && maxConnectOnMserverByLicense > 0) {
        GlobalMonitor::Instance().setThreshold(ResourceType::READER, maxConnectOnMserverByLicense, (int)(maxConnectOnMserverByLicense * 1.1));
    } else {
        GlobalMonitor::Instance().setThreshold(ResourceType::READER, -1, -1);
    }

    // monitor threshold config
#define GET_THRESHOLD(type, name)                                                                                                                              \
    double levelLow_##type = !data["thresholdConfig"].isNull() ? data["thresholdConfig"][#name "_levelLow"].asDouble() : -1;                                   \
    double levelMedium_##type = !data["thresholdConfig"].isNull() ? data["thresholdConfig"][#name "_levelMedium"].asDouble() : -1;                             \
    GlobalMonitor::Instance().setThreshold(ResourceType::type, levelLow_##type, levelMedium_##type);
    GET_THRESHOLD(CPU, CPU);
    GET_THRESHOLD(MEMORY, RAM);
    GET_THRESHOLD(HDD, STORAGE);

    // restart service config
    if (!data["restartConfig"].isNull()) {
        const Json::Value &rc = data["restartConfig"];
        RestartSchedulerConfig cfg;
        cfg.enabled    = !rc["enabled"].isNull()    ? rc["enabled"].asBool()       : false;
        cfg.type       = !rc["type"].isNull()       ? rc["type"].asString()        : "";
        cfg.time       = !rc["time"].isNull()       ? rc["time"].asString()        : "";
        cfg.dayOfWeek  = !rc["dayOfWeek"].isNull()  ? rc["dayOfWeek"].asString()   : "";
        cfg.everyHours = !rc["everyHours"].isNull() ? rc["everyHours"].asString()  : "";
        cfg.timezone   = !rc["timezone"].isNull()   ? rc["timezone"].asString()    : "";
        GlobalMonitor::Instance().setRestartConfig(cfg);
    }
}

static void loadServerClusterFromJson(const Json::Value &data) {
    // std::unordered_set<std::string> new_ids;
    // GET_CONFIG(std::string, mediaServerId, General::kMediaServerId);
     
    // std::vector<string> list_ids = ClusterManager::Instance().getListMediaServerIds(false);
    // for (const auto &server_info : data) {
    //     //WarnL << data.toStyledString();

    //     std::string active_id = server_info["id"].asString();

    //     // add active server
    //     ClusterManager::Instance().addMediaServer(server_info);

    //     // remove active server from list
    //     list_ids.erase(std::remove(list_ids.begin(), list_ids.end(), active_id), list_ids.end());
    // }

    // // remove in-active server
    // for (const auto &id : list_ids) {
    //     ClusterManager::Instance().delServer(id);
    // }

    // // Check thông luồng
    // ClusterManager::Instance().healthCheck();

    // // addMediaServer: thêm vào server
    // ClusterManager::Instance().saveServerInfoToESC();

    // // Đồng bộ db sau khi có danh sách cluster
    // //AntiEntropyManager::Instance().pullEscStateFromCluster();
    // AntiEntropyManager::Instance().checkMultiDiffAndSync();
}

static Json::Value exampleJson() {
    Json::Value data;
    data["devices"] = Json::arrayValue;
    Json::Value device;
    device["id"] = "5abab589-88ec-450a-9096-e68fcbfa84fb";
    device["name"] = "Camera HPG";
    device["username"] = "admin";
    device["password"] = "Haiphong2025";
    device["manufacturer"] = "Hikvision";
    device["model"] = "DS-2CD2347G1-L";
    device["enabled"] = true;
    device["ip"] = "27.72.173.71";
    device["httpPort"] = 80;
    device["recordingConfig"] = Json::objectValue;
    device["recordingConfig"]["enableRecording"] = true;
    Json::Value schedule = Json::arrayValue;
    for (int d = 0; d < 7; ++d) {
        for (int h = 0; h < 24; ++h) {
            Json::Value period;
            period["dh"] = StrPrinter << d << "," << h;
            period["fps"] = 25;
            period["q"] = "L";
            period["ty"] = static_cast<int>(RecordMode::RecordAlways);
            // period["ty"] = static_cast<int>(RecordMode::RecordLowResAndMotion);
            // period["ty"] = static_cast<int>(RecordMode::RecordOnlyMotion);
            schedule.append(period);
        }
    }
    device["recordingConfig"]["recordingSchedule"] = schedule;
    device["recordingConfig"]["keepArchivedMinForAuto"] = true;
    device["recordingConfig"]["keepArchivedMinFor"] = 0; // hour
    device["recordingConfig"]["keepArchivedMaxForAuto"] = false;
    device["recordingConfig"]["keepArchivedMaxFor"] = 168; // hour
    device["recordingConfig"]["motionPreRecordSec"] = 5;  // second
    device["recordingConfig"]["motionPostRecordSec"] = 5; // second
    device["cameraAdvanceConfig"] = Json::objectValue;
    device["cameraAdvanceConfig"]["streamSettings"] = Json::objectValue;
    device["cameraAdvanceConfig"]["streamSettings"]["keepConfigProfileAndStream"] = false;
    device["cameraAdvanceConfig"]["streamSettings"]["disableMainStream"] = false;
    device["cameraAdvanceConfig"]["streamSettings"]["disableSubStream"] = false;
    device["cameraAdvanceConfig"]["streamSettings"]["notRecordSubStream"] = false;
    device["cameraAdvanceConfig"]["streamSettings"]["disableAudio"] = false;
    device["cameraAdvanceConfig"]["onvif"] = Json::objectValue;
    device["cameraAdvanceConfig"]["onvif"]["mainStream"] = "AUTO";
    device["cameraAdvanceConfig"]["onvif"]["subStream"] = "AUTO";
    device["cameraAdvanceConfig"]["mediaStreaming"] = Json::objectValue;
    device["cameraAdvanceConfig"]["mediaStreaming"]["mediaPort"] = 5555;
    device["cameraAdvanceConfig"]["mediaStreaming"]["useDefaultMediaPort"] = true;
    device["cameraAdvanceConfig"]["mediaStreaming"]["rtpTransport"] = "AUTO";
    device["cameraAdvanceConfig"]["webPage"] = Json::objectValue;
    device["cameraAdvanceConfig"]["webPage"]["webPort"] = 8080;
    device["cameraAdvanceConfig"]["webPage"]["useDefaultWebPort"] = false;
    device["cameraAdvanceConfig"]["ptzSetting"]["enablePTZControl"] = true;
    device["cameraAdvanceConfig"]["ptzSetting"]["reversePanAxis"] = false;
    device["cameraAdvanceConfig"]["ptzSetting"]["reverseTiltAxis"] = true;
    device["cameraAdvanceConfig"]["ptzSetting"]["ptzSpeed"] = 0.5;
    device["cameraAdvanceConfig"]["ptzSetting"]["ptzModeSelected"] = "AUTO";
    device["priMediaServerId"] = mINI::Instance()[General::kMediaServerId];
    device["primaryStreamId"] = "0aa9322f-c0a3-4518-8273-8a7df3d35ede";
    // device["primaryStreamUrl"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile1/media.smp"; // JPEG
    device["primaryStreamUrl"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile2/media.smp";
    // device["primaryStreamUrl"] = "rtsp://viettel:Viettel@123@14.224.218.88:558/LiveChannel/3/media.smp/profile=2";
    device["secondaryStreamId"] = "56c14e52-e578-40c3-8b50-d7c315a36456";
    device["secondaryStreamUrl"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile5/media.smp";
    // device["secondaryStreamUrl"] = "rtsp://admin:Admin123@14.224.218.88:557/profile3/media.smp";
    device["motionDetectConfig"]["numOfRow"] = 32;
    device["motionDetectConfig"]["numOfColumn"] = 44;
    // device["motionDetectConfig"]["chooseStream"] = "PRIMARY";
    device["motionDetectConfig"]["chooseStream"] = "SECONDARY";
    device["motionDetectConfig"]["clientEnabled"] = false;
    device["motionDetectConfig"]["mediaSupport"] = true;
    device["motionDetectConfig"]["value"] =
        "00000000000000000000000000000000000000000000"  // row  0
        "00000000000000000000000000000000000000000000"  // row  1
        "00000000000000000000000000000000000000000000"  // row  2
        "00000000000000000000000000000000000000000000"  // row  3
        "00000000000000000000000000000000000000000000"  // row  4
        "00000000000000000000000000000000000000000000"  // row  5
        "00000000000000000000000000000000000000000000"  // row  6
        "00000000000000000000000000000000000000000000"  // row  7
        "00000000000000000000000000000000000000000000"  // row  8
        "00000000003333333333000000000000000000000000"  // row  9
        "00000000003333333333330000000000000000000000"  // row 10
        "00000000033333333333333333300000000000000000"  // row 11
        "00000000033333333333333333300000000000000000"  // row 12
        "00000000033333333333333333300000000000000000"  // row 13
        "00000000033333333333333333300000000000000000"  // row 14
        "00000003333333333333333333333330000000000000"  // row 15
        "00000003333333333333333333333330000000000000"  // row 16
        "00000003333333333333333333333333000000000000"  // row 17
        "00000033333333333333333333333333000000000000"  // row 18
        "00000033333333333333333333333333000000000000"  // row 19
        "00000333333333333333333333333333333300000000"  // row 20
        "00000333333333333333333333333333333300000000"  // row 21
        "00000333333333333333333333333333333300000000"  // row 22
        "00000333333333333333333333333333333300000000"  // row 23
        "00000333333333333333333333333333333300000000"  // row 24
        "00003333333333333333333333333333333330000000"  // row 25
        "00003333333333333333333333333333333330000000"  // row 26
        "00003333333333333333333333333333333330000000"  // row 27
        "00003333333333333333333333333333333333300000"  // row 28
        "00003333333333333333333333333333333333300000"  // row 29
        "00003333333333333333333333333333333333300000"  // row 30
        "00003333333333333333333333333333333333330000"  // row 31
        ;
    // device["motionDetectConfig"]["value"] = "";

    data["devices"].append(device);

    data["media_server"] = Json::objectValue;
    data["media_server"]["restartConfig"] = Json::objectValue;
    data["media_server"]["restartConfig"]["enabled"] = true;
    data["media_server"]["restartConfig"]["type"] = "DAILY";
    data["media_server"]["restartConfig"]["time"] = "21:38";
    data["media_server"]["restartConfig"]["dayOfWeek"] = "";
    data["media_server"]["restartConfig"]["everyHours"] = "";
    data["media_server"]["restartConfig"]["timezone"] = Json::nullValue;

    return data;
}

void loadServerConfigJson(const Json::Value &data1) {
    auto data = exampleJson();
    TraceL << "Server configuration loaded: " << data.toStyledString();
    Ticker _ticker;

    if (data.isMember("media_server")) {
        loadServerConfigFromJson(data["media_server"]);
    }

    if (data.isMember("list_media_server") && data["list_media_server"].isArray()) {
        loadServerClusterFromJson(data["list_media_server"]);
    }

    if (data.isMember("devices") && data["devices"].isArray()) {
        // get vector of current camera key 
        auto current_cameras = CameraManager::Instance().getCameraKeys();

        for (const auto &camera : data["devices"]) {
            // Get camera config from json data
            DeviceTuple tuple;
            fromJson(tuple, camera);
            CameraOption option;
            fromJson(option, camera);
            unordered_map<int, StreamTuple> stream_map;
            fromJson(stream_map, camera);

            // Add or update camera config
            CameraManager::Instance().addCamera(tuple, option, stream_map);

            // Remove active camera key from vector
            current_cameras.erase(std::remove(current_cameras.begin(), current_cameras.end(), tuple.shortUrl()), current_cameras.end());
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
    item["streamId"] = tuple.stream_id;
    item["status"] = info.live;
    item["errMsg"] = info.status;
    item["hasVideo"] = info.has_video;
    item["vcodec"] = info.vcodec;
    item["width"] = info.width;
    item["height"] = info.height;
    item["bitrate"] = info.bitrate;
    item["fps"] = info.fps;
    item["hasAudio"] = info.has_audio;
    item["acodec"] = info.acodec;
    item["channelNo"] = info.channel_no;
    item["sampleRate"] = info.sample_rate;
    item["sampleBit"] = info.sample_bit;
    item["byteSpeed"] = info.byte_speed;
    return item;
}

/**
 * Check camera online status based on stream status, if at least one stream is online then the camera is considered online
 */
static bool isGenericRtspCameraOnline(const Json::Value &device) {
    bool is_online = false;
    if (!device["primaryStreamId"].isNull() && !device["primaryStream"].isNull()) {
        auto stream = device["primaryStream"];
        if (stream["status"].asBool() == true) {
            is_online = true;
        }
    }
    if (!device["secondaryStreamId"].isNull() && !device["secondaryStream"].isNull()) {
        auto stream = device["secondaryStream"];
        if (stream["status"].asBool() == true) {
            is_online = true;
        }
    }
    return is_online;
}

/**
 * Get camera error message based on stream status, if at least one stream is online then the error message of primary stream will be shown if available, otherwise show error message of secondary stream
 */
static std::string getGenericRtspCameraErrMsg(const Json::Value &device) {
    std::string errMsg = "No message";
    // Camera can cause both primary and secondary stream are offline,
    // in this case we will show error message of primary stream if available,
    // otherwise show error message of secondary stream
    if (!device["primaryStreamId"].isNull() && !device["primaryStream"].isNull()) {
        auto stream = device["primaryStream"];
        if (stream["status"].asBool() == false) {
            errMsg = stream["errMsg"].asString();
        }
    } else if (!device["secondaryStreamId"].isNull() && !device["secondaryStream"].isNull()) {
        auto stream = device["secondaryStream"];
        if (stream["status"].asBool() == false) {
            errMsg = stream["errMsg"].asString();
        }
    }
    return errMsg;
}

void getServerStatisticJson(const function<void(Json::Value &data)> &cb) {
    Json::Value data = Json::arrayValue;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &device) {
        auto weak_listener = device->getListener();
        if (auto strong_listener = weak_listener.lock()) {
            auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
            if (impl) {
                auto camera = dynamic_pointer_cast<GenericRtspCamera>(device);
                auto stats_imp = impl->getCameraStatisticImp();
                if (stats_imp) {
                    auto params = stats_imp->getParams();
                    auto option = params.option;
                    if (option.enableFailover && !impl->isEnabled()) {
                        // this camera run in failover mode and actual camera connection run on prefered media server
                        return;
                    }
                    Json::Value item;
                    item["deviceId"] = params.tuple.device_id;
                    // Get both stream status
                    if (camera->hasStreamTuple(PrimaryStream)) {
                        item["primaryStreamId"] =  params.stream_map[PrimaryStream].stream_id;
                        item["primaryStream"] = makeStreamStatisticJson(params, PrimaryStream);
                    } else {
                        item["primaryStreamId"] = Json::nullValue;
                        item["primaryStream"] = Json::nullValue;
                    }

                    if (camera->hasStreamTuple(SecondaryStream)) {
                        item["secondaryStreamId"] =  params.stream_map[SecondaryStream].stream_id;
                        item["secondaryStream"] = makeStreamStatisticJson(params, SecondaryStream);
                    } else {
                        item["secondaryStreamId"] = Json::nullValue;
                        item["secondaryStream"] = Json::nullValue;
                    }
                    // note: for generic rtsp camera, we will determine camera online status based on stream status, if at least one stream is online then the camera is considered online, and error message will be determined based on stream status as well,
                    // if at least one stream is online then the error message of primary stream will be shown if available, otherwise show error message of secondary stream; for onvif camera, we will determine camera online status based on device connection status, and error message will be determined based on device connection status as well
                    auto is_online = isGenericRtspCameraOnline(item);
                    item["status"] = is_online;
                    item["errMsg"] = is_online ? "Connected" : getGenericRtspCameraErrMsg(item);
                    // Get controller status
                    if (option.manufacturer != GENERIC_RTSP_CAMERA && !option.manufacturer.empty()) {
                        item["controller"]["status"] = params.device_stats.connect;
                        item["controller"]["errMsg"] = params.device_stats.status;
                    } else {
                        item["controller"] = Json::nullValue;
                    }
                    data.append(item);
                }
            }
        }
    });
    TraceL << "Server statistic report: " << data.toStyledString();
    cb(data);
}

static Json::Value getPTZModeString(bool isAbsolute, bool isRelative, bool isContinuous) {
    Json::Value ret = Json::arrayValue;
    ret.append("AUTO");
    if (isAbsolute) {
        ret.append("ABSOLUTE");
    }
    if (isRelative) {
        ret.append("RELATIVE");
    }
    if (isContinuous) {
        ret.append("CONTINUOUS");
    }
    return ret;
}

static Json::Value getOnvifProfileJsonArray(const std::vector<OnvifMediaProfile> &profiles) {
    Json::Value ret = Json::arrayValue;
    for (const auto &profile : profiles) {
        Json::Value profileJson = Json::objectValue;
        profileJson["token"] = profile.token;
        profileJson["url"] = profile.url;
        profileJson["hasVideo"] = profile.hasVideo;
        profileJson["vcodec"] = profile.vcodec;
        profileJson["width"] = profile.width;
        profileJson["height"] = profile.height;
        profileJson["bitrate"] = profile.bitrate;
        profileJson["fps"] = profile.fps;
        profileJson["hasAudio"] = profile.hasAudio;
        profileJson["acodec"] = profile.acodec;
        profileJson["channelNo"] = profile.channelNo;
        profileJson["sampleBit"] = profile.sampleBit;
        profileJson["sampleRate"] = profile.sampleRate;
        ret.append(profileJson);
    }
    return ret;
}

Json::Value makeDeviceCapabilitiesJson(const DeviceSource::Ptr &device, const DeviceCapabilities* caps) {
    Json::Value data;
    auto weak_listener = device->getListener();
    if (auto strong_listener = weak_listener.lock()) {
        auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
        if (impl) {
            auto option = impl->getCameraOption();
            data["deviceId"] = device->getDeviceTuple().device_id;
            if (caps->isOnvifDevice) {
                data["onvifDevice"] = true;
                auto deviceInfo = caps->onvifProfile.deviceInfo;
                data["manufacturer"] = deviceInfo.manufacturer;
                data["model"] = deviceInfo.model;
                data["serialNumber"] = deviceInfo.serialNumber;
                data["firmwareVersion"] = deviceInfo.firmwareVersion;
                data["hardwareId"] = deviceInfo.hardwareId;
                data["macAddress"] = deviceInfo.macAddress;
                data["hasWebPage"] = true;
                data["webPage"] = StrPrinter << "http://" << option.ip << ":" << (option.autoWebPort ? option.port : option.webPort);
                
                Json::Value onvifProfileJson = Json::objectValue;
                auto ptzProfile = caps->onvifProfile.ptzProfile;
                onvifProfileJson["isPTZ"] = ptzProfile.isAbsMoveEnable || ptzProfile.isRelMoveEnable || ptzProfile.isConsMoveEnable;
                onvifProfileJson["ptzControlMode"] = getPTZModeString(ptzProfile.isAbsMoveEnable, ptzProfile.isRelMoveEnable, ptzProfile.isConsMoveEnable);
                auto mediaProfiles = caps->onvifProfile.mediaProfiles;
                onvifProfileJson["profiles"] = getOnvifProfileJsonArray(mediaProfiles);
                data["onvifProfiles"] = onvifProfileJson;
            } else {
                data["manufacturer"] = !option.manufacturer.empty() ? option.manufacturer : GENERIC_RTSP_CAMERA;
                data["model"] = !option.model.empty() ? option.model : GENERIC_RTSP_CAMERA;
                data["serialNumber"] = "";
                data["firmwareVersion"] = "";
                data["hardwareId"] = "";
                data["macAddress"] = "";
                data["hasWebPage"] = false;
                data["webPage"] = "";
                data["onvifDevice"] = false;
                Json::Value onvifProfileJson = Json::objectValue;
                onvifProfileJson["isPTZ"] = false;
                onvifProfileJson["ptzControlMode"] = Json::arrayValue;
                onvifProfileJson["profiles"] = Json::arrayValue;
                data["onvifProfiles"] = onvifProfileJson;
            }
#ifdef ENABLE_MOTION
            data["motionDetection"]["mediaSupport"] = true;
#else
            data["motionDetection"]["mediaSupport"] = false;
#endif
        }
    }
    return data;
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
    auto weak_listener = device->getListener();
    if (auto strong_listener = weak_listener.lock()) {
        auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
        if (impl) {
            auto stats_imp = impl->getCameraStatisticImp();
            if (stats_imp) {
                auto stats = stats_imp->getParams();
                ret["id"] = stats.tuple.device_id;
                ret["name"] = stats.option.name;
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
    }, GENERIC_RTSP_CAMERA_SCHEMA);

    data["totalMainDevice"] = server_stats.totalMainDevice;
    data["totalMainBitrate"] = server_stats.totalMainBitrate;
    data["totalMainUsedStorage"] = server_stats.totalMainUsedStorage;
    data["totalFailoverDevice"] = server_stats.totalFailoverDevice;
    data["totalFailoverBitrate"] = server_stats.totalFailoverBitrate;
    data["totalFailoverUsedStorage"] = server_stats.totalFailoverUsedStorage;
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
        bool is_online = isGenericRtspCameraOnline(device);
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

Json::Value makeCameraOptionJson(const CameraOption &option) {
    Json::Value val;
    // Basic info
    val["name"]         = option.name;
    val["manufacturer"] = option.manufacturer;
    val["model"]        = option.model;
    val["ip"]           = option.ip;
    val["port"]         = option.port;
    val["username"]     = option.username;
    val["password"]     = option.password;

    // Stream control
    val["disablePrimaryStream"]      = option.disablePrimaryStream;
    val["disableSecondaryStream"]    = option.disableSecondaryStream;
    val["doNotRecordPrimaryStream"]  = option.doNotRecordPrimaryStream;
    val["doNotRecordSecondaryStream"]= option.doNotRecordSecondaryStream;
    val["disableAudio"]              = option.disableAudio;

    // Recording
    val["enableRecord"]            = option.enableRecord;
    val["keepArchivedMinForAuto"]  = option.keepArchivedMinForAuto;
    val["keepArchivedMinFor"]      = (Json::UInt64)option.keepArchivedMinFor;
    val["keepArchivedMaxForAuto"]  = option.keepArchivedMaxForAuto;
    val["keepArchivedMaxFor"]      = (Json::UInt64)option.keepArchivedMaxFor;
    val["motionPreRecordSec"]      = option.motionPreRecordSec;
    val["motionPostRecordSec"]     = option.motionPostRecordSec;
    val["recordSchedules"]         = option.recordSchedules;

    // Device activation & media transport
    val["enableActive"]            = option.enableActive;
    val["mediaPort"]               = option.mediaPort;
    val["autoMediaPort"]           = option.autoMediaPort;
    val["rtpTransport"]            = option.rtpTransport;
    val["enableFailover"]          = option.enableFailover;
    val["preferedMediaServer"]     = option.preferedMediaServer;

    // Web access
    val["webPort"]                 = option.webPort;
    val["autoWebPort"]             = option.autoWebPort;
    val["keepConfigProfileAndStream"] = option.keepConfigProfileAndStream;

    // PTZ
    val["enablePTZControl"]    = option.enablePTZControl;
    val["reversePanAxis"]      = option.reversePanAxis;
    val["reverseTiltAxis"]     = option.reverseTiltAxis;
    val["ptzMode"]             = option.ptzMode;
    val["ptzSpeed"]            = option.ptzSpeed;
    val["onvifMainProfile"]    = option.onvifMainProfile;
    val["onvifSubProfile"]     = option.onvifSubProfile;

    // Motion detection
    val["enableMotion"]         = option.enableMotion;
    val["roiValue"]             = option.roiValue;
    val["motionDetectOnStream"] = option.motionDetectOnStream;

    return val;
}

Json::Value makeDeviceStatisticJson(const DeviceSource::Ptr &device) {
    Json::Value item;
    auto weak_listener = device->getListener();
    if (auto strong_listener = weak_listener.lock()) {
        auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
        if (impl) {
            auto camera = impl->getCameraSource();
            auto stats_imp = impl->getCameraStatisticImp();
            if (stats_imp) {
                auto params = stats_imp->getParams();
                auto option = params.option;
                item["deviceId"] = params.tuple.device_id;
                // Get both stream status
                if (camera->hasStreamTuple(PrimaryStream)) {
                    item["primaryStreamId"] =  params.stream_map[PrimaryStream].stream_id;
                    item["primaryStream"] = makeStreamStatisticJson(params, PrimaryStream);
                } else {
                    item["primaryStreamId"] = Json::nullValue;
                    item["primaryStream"] = Json::nullValue;
                }

                if (camera->hasStreamTuple(SecondaryStream)) {
                    item["secondaryStreamId"] =  params.stream_map[SecondaryStream].stream_id;
                    item["secondaryStream"] = makeStreamStatisticJson(params, SecondaryStream);
                } else {
                    item["secondaryStreamId"] = Json::nullValue;
                    item["secondaryStream"] = Json::nullValue;
                }
                // note: for generic rtsp camera, we will determine camera online status based on stream status, if at least one stream is online then the camera is considered online, and error message will be determined based on stream status as well,
                // if at least one stream is online then the error message of primary stream will be shown if available, otherwise show error message of secondary stream; for onvif camera, we will determine camera online status based on device connection status, and error message will be determined based on device connection status as well
                auto is_online = isGenericRtspCameraOnline(item);
                item["status"] = is_online;
                item["errMsg"] = is_online ? "Connected" : getGenericRtspCameraErrMsg(item);
                // Get controller status
                if (option.manufacturer != GENERIC_RTSP_CAMERA && !option.manufacturer.empty()) {
                    item["controller"]["status"] = params.device_stats.connect;
                    item["controller"]["errMsg"] = params.device_stats.status;
                } else {
                    item["controller"] = Json::nullValue;
                }
                item["options"] = makeCameraOptionJson(option);
            }
        }
    }
    return item;
}

Json::Value makeDevicePTZPresetJson(const DeviceSource::Ptr &device) {
    Json::Value item;
    auto weak_listener = device->getListener();
    if (auto strong_listener = weak_listener.lock()) {
        auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
        if (impl) {
            auto camera = impl->getCameraSource();
            auto stats_imp = impl->getCameraStatisticImp();
            if (stats_imp) {
                auto params = stats_imp->getParams();
                if (params.device_stats.device_caps.onvifProfile.ptzProfile.isPresetEnable) {
                    for (auto &it : params.device_stats.device_caps.onvifProfile.ptzProfile.presetMap) {
                        Json::Value preset;
                        preset["name"] = it.second.Name;
                        preset["token"] = it.second.Token;
                        preset["isUserDefined"] = false;
                        item.append(preset);
                    }

                    for (auto &it : params.device_stats.user_presets) {
                        Json::Value preset;
                        preset["name"] = it.second.Name;
                        preset["token"] = it.second.Token;
                        preset["isUserDefined"] = true;
                        item.append(preset);
                    }
                }
            }
        }
    }
    return item;
}