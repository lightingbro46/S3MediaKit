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
#include "Storage/MiscData.h"
#include "Local/StorageManager.h"
#include "Local/TierStorageManager.h"
#include "Server/GlobalMonitor.h"
#include "Manager.h"
#include "Server/ClusterManager.h"
#include "Local/StatisticRecorder.h"
#include "Common/StrUtil.h"
#include "User/UserAuditLog.h"
#include "User/UserAuthorManager.h"
#include "Extension/SyncManager.h"
#include "Transcode/OverlayPrivacyUtils.h"

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
const string kFailoverActiveDelaySec = MANAGER_FIELD"failoverActiveDelaySec";
const string kJwtPublicKey = MANAGER_FIELD"jwtPublicKey";
const string kSessionExpiryDays = MANAGER_FIELD"sessionExpiryDays";
const string kMaxStreamTimeoutSec = MANAGER_FIELD"maxStreamTimeoutSec";
const string kBypassAuthRealm = MANAGER_FIELD"bypassAuthRealm";
const string kApiSecret = MANAGER_FIELD"apiSecret";

static onceToken token([]() {
    mINI::Instance()[kMediaServerDomain] = "";
    mINI::Instance()[kMediaServerProjectId] = "";
    mINI::Instance()[kMaxAllowedDevices] = 0;
    mINI::Instance()[kMaxAvailableDevices] = 256;
    mINI::Instance()[kServerLocationId] = 1;
    mINI::Instance()[kEnableFailover] = false;
    mINI::Instance()[kFailoverActiveDelaySec] = 1800;
    mINI::Instance()[kEnableAuthorize] = true;
    mINI::Instance()[kJwtPublicKey] = "";
    mINI::Instance()[kSessionExpiryDays] = 180;
    mINI::Instance()[kMaxStreamTimeoutSec] = 10.0;
    mINI::Instance()[kBypassAuthRealm] = "";
    mINI::Instance()[kApiSecret] = "";
});
} // namespace Manager

} // namespace managerkit

static void enforceStoragePolicy() {
    DebugL << "Storage manager has been started monitoring";
    StorageManager::Instance().start();
#ifdef ENABLE_TIER_STORAGE
    TierStorageManager::Instance().start();
#endif
}

static void loadSavedDeviceInfo() {
    EventPollerPool::Instance().getPoller()->doDelayTask(3000, []() {
        DebugL << "Camera manager has been started loading saved camera";
        CameraManager::Instance().loadSavedCameraInfo();
        return 0;
    });
    EventPollerPool::Instance().getPoller()->doDelayTask(3000, []() {
        DebugL << "Speaker manager has been started loading saved speaker";
        SpeakerManager::Instance().loadSavedSpeakerInfo();
        return 0;
    });
}

static void loadSavedMediaServerInfo() {
    EventPollerPool::Instance().getPoller()->doDelayTask(6000, []() {
        DebugL << "Cluster manager has been started loading persisted peers";
        ClusterManager::Instance().loadSavedMediaServerInfo();
        DebugL << "Sync manager has been started";
        SyncManager::Instance().start();
        return 0;
    });
}

static void *manager_hook_tag = nullptr;

#ifdef ENABLE_MP4
static std::string resolveRecordedBlockPath(const TimeBlock &block) {
    std::string timefile_path;
    if (!block.file_path().empty())
        timefile_path = decodeBase64(block.file_path());
    if (timefile_path.empty())
        return "";

#ifdef ENABLE_TIER_STORAGE
    auto resolved = TierStorageManager::Instance().resolvePlaybackSegmentPath(
        block.app(),
        block.stream(),
        static_cast<int64_t>(block.start_time()),
        timefile_path);

    if (resolved.ready && !resolved.read_path.empty())
        return resolved.read_path;

    if (resolved.restore_required) {
        auto restore = TierStorageManager::Instance().handleColdAccessByPath(timefile_path);
        WarnL << "Recorded MP4 segment requires restore before playback, camera=" << block.app()
              << " stream=" << block.stream()
              << " start_time=" << block.start_time()
              << " pool=" << resolved.pool_id
              << " range=" << resolved.range_id
              << " restore_job=" << restore.job_id
              << " status=" << restore.status;
    } else {
        WarnL << "Recorded MP4 segment path is not ready, camera=" << block.app()
              << " stream=" << block.stream()
              << " start_time=" << block.start_time()
              << " message=" << resolved.message;
    }
    return "";
#else
    return timefile_path;
#endif
}
#endif

void installManagerHook () {

#ifdef ENABLE_MP4
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        DebugL << "Record mp4 file " << info.app << "/" << info.stream << "/" << info.start_time << "/" << info.time_len << "/" << info.file_path;
        TimeBlock block;
        const bool isReplay = start_with(info.app, kReplayPrefix);
        std::string app = isReplay ? info.app.substr(kReplayPrefix.size()) : info.app;
        block.set_app(app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(round(info.time_len));
        block.set_file_size(info.file_size);
        block.set_is_replay(isReplay);
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
        std::map<std::string, std::map<uint64_t, std::string>> files;
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
                // Use first_range.duration — already a merged, de-duplicated span
                // covering all streams.  Summing block.time_len() would double-count
                // when both hi and lo blocks exist for the same timestamps.
                duration = first_range.duration;
                query->getRecordedTimePeriod(first_range.startTime, first_range.startTime + first_range.duration, [&](vector<TimeBlock> &ret) {
                    for (const auto &block : ret) {
                        auto resolved_path = resolveRecordedBlockPath(block);
                        if (!resolved_path.empty())
                            files[block.stream()][block.start_time()] = resolved_path;
                    }
                });
            }
        }
        invoker(duration, files);
    });

    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastMediaViewOverlay, [](BroadcastMediaViewOverlayArgs) {
        Broadcast::ViewOverlayPolicy policy;
        auto device_id = args.app;
        GET_CONFIG(string, record_app, Record::kAppName);
        if (args.app == record_app) {
            device_id = split(args.stream, "/")[0];
        }

        auto device = DeviceSource::find(args.vhost, device_id);
        if (!device) {
            invoker(policy);
            return;
        }
        auto weak_listener = device->getListener();
        auto strong_listener = weak_listener.lock();
        if (!strong_listener) {
            invoker(policy);
            return;
        }
        auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
        if (!impl) {
            invoker(policy);
            return;
        }

        const auto &option = impl->getCameraOption();
        policy.watermark_enforce = option.enforceWatermarkOnView;
        policy.watermark_template = option.watermarkTemplate;
        policy.privacy_mask_enforce = option.enforcePrivacyMaskOnView;
        policy.privacy_mask_regions = option.privacyMaskRegions;
        policy.camera_name = option.name;

        if (!policy.watermark_enforce && !policy.privacy_mask_enforce) {
            invoker(policy);
            return;
        }

        // Check user session and role to determine if watermark or privacy mask should be excluded
        if (jwt_token.empty()) {
            invoker(policy);
            return;
        }

        auto token_cache = UserAuthorManager::Instance().getTokenCache(jwt_token);
        auto role_code = token_cache->getRoleCode();
        auto overlay = token_cache->getOverlay();

        if (overlay && option.enforceWatermarkOnView && !option.watermarkExcludedRoleIds.empty()) {
            for (auto role : split(option.watermarkExcludedRoleIds, ",")) {
                trim(role);
                if (!role.empty() && role == role_code) {
                    policy.watermark_excluded = true;
                    break;
                }
            }
        } else if (!overlay) {
            policy.watermark_excluded = true;
        } else {
            policy.watermark_excluded = false;
        }
        
        if (overlay && option.enforcePrivacyMaskOnView && !option.privacyMaskExcludedRoleIds.empty()) {
            for (auto role : split(option.privacyMaskExcludedRoleIds, ",")) {
                trim(role);
                if (!role.empty() && role == role_code) {
                    policy.privacy_mask_excluded = true;
                    break;
                }
            }
        } else if (!overlay) {
            policy.privacy_mask_excluded = true;
        } else {
            policy.privacy_mask_excluded = false;
        }

        invoker(policy);
    });

    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastGetRecordedMP4, [](BroadcastGetRecordedMP4Args) {
        TimeQuery::Ptr query;
        std::map<std::string, std::map<uint64_t, std::string>> files;
        try {
            query = make_shared<TimeQuery>(args);
        } catch(...) {}
        
        if (query) {
            query->getRecordedTimePeriod(stamp, stamp + max_duration, [&](vector<TimeBlock> &ret) {
                for (const auto &block : ret) {
                    auto resolved_path = resolveRecordedBlockPath(block);
                    if (!resolved_path.empty())
                        files[block.stream()][block.start_time()] = resolved_path;
                }
            });
        }
        invoker(files);
    });

#endif // ENABLE_MP4

    // Query stream quality map (PrimaryStream→hi stream_id, SecondaryStream→lo stream_id)
    // for a device that may currently be offline. Reads info.txt via StatisticRecorder.
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastGetStreamQuality, [](BroadcastGetStreamQualityArgs) {
        std::map<int, std::string> result;
        auto recorder = StatisticRecorder::Instance().getRecorder(device_id, false);
        if (recorder) {
            auto params = recorder->getParams();
            for (const auto &kv : params.stream_map) {
                if (!kv.second.stream_id.empty()) {
                    result[kv.first] = kv.second.stream_id;
                }
            }
        }
        invoker(result);
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

    loadSavedMediaServerInfo();
}

static void releaseAllDevice() {
    // release all camera
    CameraManager::Instance().clear();
    // release all speaker
    SpeakerManager::Instance().clear();
}

static void releaseSyncDatabase() {
    SyncManager::Instance().stop();
}

void unInstallManagerHook() {
    releaseSyncDatabase();
    releaseAllDevice();

    // sleep for 10 second before uninstall hook, to prevent resource release order errors
    sleep(10);

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

    TraceL << "Init misc data";
    auto miscDataImp = make_shared<MiscDataImp>();
    miscDataImp->initMiscData();
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
    // todo: we can optimize the logic of emitStreamStatusChangeEvent by adding a specific field 
    // in the request to indicate whether to emit the event, instead of relying on the name or manufacturer containing "VIDEO PUSH"
    // option.emitStreamStatusChangeEvent = option.name.find("VIDEO PUSH") != string::npos || option.manufacturer.find("VIDEO PUSH") != string::npos; 

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
        if (option.motionPreRecordSec <= 5) {
            option.motionPreRecordSec = 5;
        }
        GET_OPTION_PROPERTY(option, motionPostRecordSec, rc, motionPostRecordSec)
        if (option.motionPostRecordSec <= 5) {
            option.motionPostRecordSec = 5;
        }
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

    if (data.isMember("sdCardSyncConfig") && !data["sdCardSyncConfig"].isNull()) {
        const Json::Value &cfg = data["sdCardSyncConfig"];
        option.sdCardSyncEnabled = cfg["syncEnabled"].asBool();
        option.sdCardSyncAutoSyncEnabled = cfg["autoSyncEnabled"].asBool();
        option.sdCardSyncMinSegmentGapSec = cfg["minSegmentGapSec"].asInt();
        option.sdCardSyncRetryCount = cfg["retryCount"].asInt();
    }

    if (data.isMember("privacyConfig") && !data["privacyConfig"].isNull()) {
        const Json::Value &pc = data["privacyConfig"];

        GET_OPTION_PROPERTY(option, enforcePrivacyMaskOnView, pc, enabled)
        std::ostringstream ss;
        for (const auto &roleId : pc["excludedRoleIds"]) {
            ss << roleId.asString() << ",";
        }
        std::string excludedRoleIds = ss.str();
        option.privacyMaskExcludedRoleIds = excludedRoleIds.empty() ? "" : excludedRoleIds.substr(0, excludedRoleIds.size() - 1);
        GET_OPTION_PROPERTY_AS_STRING(option, privacyMaskRegions, pc, regions)
    }

    if (data.isMember("watermarkTemplateId") && !data["watermarkTemplateId"].isNull() ) {
        GET_OPTION_PROPERTY(option, watermarkTemplateId, data, watermarkTemplateId)
        std::ostringstream ss;
        for (const auto &roleId : data["watermarkExcludedRoleIds"]) {
            ss << roleId.asString() << ",";
        }
        std::string excludedRoleIds = ss.str();
        option.watermarkExcludedRoleIds = excludedRoleIds.empty() ? "" : excludedRoleIds.substr(0, excludedRoleIds.size() - 1);
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

static void fromJson(SpeakerOption &option, const Json::Value &data) {
    GET_OPTION_PROPERTY(option, name, data, name)
    GET_OPTION_PROPERTY(option, manufacturer, data, manufacturer)
    GET_OPTION_PROPERTY(option, model, data, model)
    GET_OPTION_PROPERTY(option, username, data, username)
    GET_OPTION_PROPERTY(option, password, data, password)
    GET_OPTION_PROPERTY(option, ip, data, ip)
    GET_OPTION_PROPERTY(option, deviceUsername, data, deviceUsername)
    GET_OPTION_PROPERTY(option, devicePassword, data, devicePassword)
    // http port default 80
    GET_OPTION_PROPERTY_OR_DEFAULT_VALUE(option, port, data, httpPort, 80)
    GET_OPTION_PROPERTY_OR_DEFAULT_VALUE(option, devicePort, data, devicePort, 80)
    GET_OPTION_PROPERTY(option, enableActive, data, enabled)
    GET_OPTION_PROPERTY(option, preferedMediaServer, data, priMediaServerId)
    GET_CONFIG(string, mediaServerId, General::kMediaServerId)
    if (data.isMember("vendorFeaturesConfig") && !data["vendorFeaturesConfig"].isNull()) {
        const Json::Value &vfc = data["vendorFeaturesConfig"];

        GET_OPTION_PROPERTY(option, enableVendorFeature, vfc, enableVendorFeature)
        GET_OPTION_PROPERTY(option, separateCredentialConfigured, vfc, separateCredentialConfigured)

        if (vfc.isMember("enabledVendorFeatures") && !vfc["enabledVendorFeatures"].isNull()) {
            for (const auto &feature : vfc["enabledVendorFeatures"]) {
                option.enabledVendorFeatures.push_back(feature.asString());
            }
        }
    }
}

static void fromJson(AudioFile &file, const Json::Value &data) {
    file.id = data["id"].asString();
    file.name = data["fileName"].asString();
    file.size = data["size"].asUInt64();
    file.soundPath = data["soundPath"].asString();
    file.duration = data["duration"].asFloat();
    file.createdAt = data["createdAt"].asString();
    file.updatedAt = data["updatedAt"].asString();
    file.downloaded = false;
}

static void loadWatermarkTemplateFromJson(unordered_map<string, string> &ret, const Json::Value &data) {
    ret.clear();
    for (const auto &w : data) {
        if (!w["id"].isNull() && !w["name"].isNull()) {
            auto id = w["id"].asString();
            auto name = w["name"].asString();
            if (!id.empty()) {
                try {
                    ret[id] = StrJsonUtils::writeJsonString(w);
                    TraceL << "Load watermark template from json: id=" << id << ", name=" << name;
                } catch (const std::exception &e) {
                    WarnL << "Failed to load watermark template from json: id=" << id << ", name=" << name << ", error=" << e.what();
                }
            }
        }
    }
}

static void prefetchWatermarkOverlayImages(const unordered_map<string, string> &watermark_templates) {
    GET_CONFIG(bool, use_watermark_asset, OverlayPrivacyConfig::kUseWatermarkAsset);
    for (const auto &entry : watermark_templates) {
        if (use_watermark_asset) {
            const string template_id = entry.first;
            OverlayPrivacyUtils::prepareWatermarkAsset(entry.second,
                [template_id](const string &err, const string &path) {
                    if (!err.empty()) {
                        WarnL << "Prefetch watermark SVG asset failed, template_id="
                              << template_id << ": " << err;
                        return;
                    }
                    TraceL << "Prefetch watermark SVG asset completed, template_id="
                           << template_id << ": " << path;
                });
            continue;
        }
        vector<OverlayComponent> components;
        OverlayBuildOptions options;
        const string template_id = entry.first;
        OverlayPrivacyUtils::prepareComponents(entry.second, components, options,
            [template_id](const string &err, const vector<OverlayComponent> &) {
                if (!err.empty()) {
                    WarnL << "Prefetch watermark overlay images failed, template_id="
                            << template_id << ": " << err;
                    return;
                }
                TraceL << "Prefetch watermark overlay images completed, template_id=" << template_id;
            });
    }
}

static void loadWatermarkTemplateFromMap(CameraOption &option,  const unordered_map<string, string> &watermark_templates) {
    if (!option.watermarkTemplateId.empty()) {
        auto it = watermark_templates.find(option.watermarkTemplateId);
        if (it != watermark_templates.end()) {
            option.watermarkTemplate = it->second;
            option.watermarkExcludedRoleIds = "";
            DebugL << "Load watermark template from map: id=" << option.watermarkTemplateId << ". Set enforceWatermarkOnView to true.";
            option.enforceWatermarkOnView = true;
        } else {
            WarnL << "Watermark template not found for id=" << option.watermarkTemplateId << ". Reset enforceWatermarkOnView to false.";
            option.enforceWatermarkOnView = false;
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
    if (!unlimitedConnectOnMserver && maxConnectOnMserver > 0 && maxConnectOnMserverByLicense > 0) {
        auto value = min(maxConnectPerCamera, maxConnectPerCameraByLicense);
        GlobalMonitor::Instance().setThreshold(ResourceType::READER, value, (int)(value * 1.1));
    } else if (unlimitedConnectOnMserver && maxConnectOnMserverByLicense > 0) {
        GlobalMonitor::Instance().setThreshold(ResourceType::READER, maxConnectOnMserverByLicense, (int)(maxConnectOnMserverByLicense * 1.1));
    } else {
        GlobalMonitor::Instance().setThreshold(ResourceType::READER, -1, -1);
    }

    // monitor threshold config
#define GET_THRESHOLD(type, name, warning_config, critical_config)                                                                                             \
    GET_CONFIG(float, defaultLevelLow_##type, GlobalMonitorConfig::warning_config);                                                                            \
    GET_CONFIG(float, defaultLevelMedium_##type, GlobalMonitorConfig::critical_config);                                                                        \
    const Json::Value &thresholdConfig_##type = data["thresholdConfig"];                                                                                       \
    float levelLow_##type = !data["thresholdConfig"].isNull() ? data["thresholdConfig"][#name "_levelLow"].asDouble() : defaultLevelLow_##type;                \
    float levelMedium_##type = !data["thresholdConfig"].isNull() ? data["thresholdConfig"][#name "_levelMedium"].asDouble() : defaultLevelMedium_##type;       \
    GlobalMonitor::Instance().setThreshold(ResourceType::type, levelLow_##type, levelMedium_##type);
    
    GET_THRESHOLD(CPU, CPU, kCpuWarningThreshold, kCpuCriticalThreshold);
    GET_THRESHOLD(MEMORY, RAM, kMemoryWarningThreshold, kMemoryCriticalThreshold);
    GET_THRESHOLD(HDD, STORAGE, kHddWarningThreshold, kHddCriticalThreshold);
#undef GET_THRESHOLD

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

static void fromJson(MediaServerInfo &info, const Json::Value &data) {
    info.id = data["id"].asString();
    info.name = data["name"].asString();
    info.domain = data["domain"].asString();
    info.ip = data["ip"].asString();
    info.httpPort = data["httpPort"].asInt();
    info.httpsPort = data["httpsPort"].asInt();
    info.rtspPort = data["rtspPort"].asInt();
    info.rtmpPort = data["rtmpPort"].asInt();
    info.isAutoHttpPort = data["isHttpPortAuto"].asBool();
    info.isAutoHttpsPort = data["isHttpsPortAuto"].asBool();
    info.isAutoRtspPort = data["isRtspPortAuto"].asBool();
    info.isAutoRtmpPort = data["isRtmpPortAuto"].asBool();
    info.natHttpPort = data["httpPortNat"].asInt();
    info.natHttpsPort = data["httpsPortNat"].asInt();
    info.natRtspPort = data["rtspPortNat"].asInt();
    info.natRtmpPort = data["rtmpPortNat"].asInt();
    info.clientUseSsl = data["clientUseSsl"].asBool();
    info.useWebDomain = !data["dynamicDomain"].empty() ? data["dynamicDomain"]["useWebDomain"].asBool() : false;
    info.useDomain = !data["dynamicDomain"].empty() ? data["dynamicDomain"]["useDomain"].asBool() : false;
    info.useCustomPath = !data["dynamicDomain"].empty() ? data["dynamicDomain"]["useCustomPath"].asBool() : false;
    info.customPath = !data["dynamicDomain"].empty() ? data["dynamicDomain"]["customPath"].asString() : "";
    info.hasFailover = data["hasFailover"].asBool();
    info.serverGroupId = data["serverGroupId"].asInt();
}

static void loadServerClusterFromJson(const Json::Value &data) {
    auto current_mserver = ClusterManager::Instance().getMediaServerIds();

    for (const auto &server : data) {
        std::string active_id = server["id"].asString();

        // add active server
        MediaServerInfo mserver;
        fromJson(mserver, server);
        ClusterManager::Instance().addMediaServer(active_id, mserver);

        // remove active server from list
        current_mserver.erase(std::remove(current_mserver.begin(), current_mserver.end(), active_id), current_mserver.end());
    }

    // remove in-active server
    for (const auto &id : current_mserver) {
        ClusterManager::Instance().removeMediaServer(id);
    }

    SyncManager::Instance().setSingleNodeMode(ClusterManager::Instance().getMediaServerIds().size() == 1);
    TraceL << "Load media server cluster config: " << ClusterManager::Instance().getMediaServerIds().size() << " active servers";
}

void loadServerConfigJson(const Json::Value &data_api) {
    Json::Value data;
#ifdef ENABLE_DEBUG
    string file_name = "api-config.json";
    if (File::fileExist(file_name)) {
        auto file_content = File::loadFile(file_name);
        if (!StrJsonUtils::readJsonString(file_content, data)) {
            WarnL << "Parse server configuration file failed: " << file_name;
            return;
        }
    }
#else
    data = data_api;
#endif
    TraceL << "Server configuration loaded: " << data.toStyledString();
    Ticker _ticker;

    if (data.isMember("media_server")) {
        loadServerConfigFromJson(data["media_server"]);
    }

    if (data.isMember("list_media_server") && data["list_media_server"].isArray()) {
        loadServerClusterFromJson(data["list_media_server"]);
    }

    unordered_map<string, string> watermark_template_map;
    if (data.isMember("watermark_configs") && data["watermark_configs"].isArray()) {
        loadWatermarkTemplateFromJson(watermark_template_map, data["watermark_configs"]);
        prefetchWatermarkOverlayImages(watermark_template_map);
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
            loadWatermarkTemplateFromMap(option, watermark_template_map);
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
    DebugL << "Server configuration for camera loaded completed, took " << _ticker.elapsedTime() << " ms";
    _ticker.resetTime();

    if (data.isMember("speakers") && data["speakers"].isArray()) {
        // get vector of current speaker key 
        auto current_speakers = SpeakerManager::Instance().getSpeakerKeys();

        for (const auto &speaker : data["speakers"]) {
            // Get speaker config from json data
            DeviceTuple tuple;
            fromJson(tuple, speaker);
            SpeakerOption option;
            fromJson(option, speaker);

            // Add or update speaker config
            SpeakerManager::Instance().addSpeaker(tuple, option);

            // Remove active speaker key from vector
            current_speakers.erase(std::remove(current_speakers.begin(), current_speakers.end(), tuple.shortUrl()), current_speakers.end());
        }

        // Remove all inactive speaker
        for (const auto &key : current_speakers) {
            SpeakerManager::Instance().delSpeaker(key);
        }
    }
    DebugL << "Server configuration for speaker loaded completed, took " << _ticker.elapsedTime() << " ms";

    if (data.isMember("list_audio_file") && data["list_audio_file"].isArray()) {
        // get vector of current audio files
        auto current_audio_list = AudioFileManager::Instance().getAllAudioFileIds();

        for (const auto &audio_file : data["list_audio_file"]) {
            // Add or update audio file config
            AudioFile file;
            fromJson(file, audio_file);
            AudioFileManager::Instance().addAudioFile(file);

            // Remove active audio file id from vector
            current_audio_list.erase(std::remove(current_audio_list.begin(), current_audio_list.end(), file.id), current_audio_list.end());
        }

        // Remove all inactive file
        for (const auto &key : current_audio_list) {
            AudioFileManager::Instance().delAudioFile(key);
        }

        AudioFileManager::Instance().syncDownload();
    }
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
        try {
            auto weak_listener = device->getListener();
            if (auto strong_listener = weak_listener.lock()) {
                if (auto cameraImp = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener)) {
                    auto camera = dynamic_pointer_cast<GenericRtspCamera>(device);
                    auto stats_imp = cameraImp->getCameraStatisticImp();
                    if (stats_imp) {
                        auto params = stats_imp->getParams();
                        auto option = params.option;
                        if (option.enableFailover && !cameraImp->isEnabled()) {
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

                if (auto speakerImp = std::dynamic_pointer_cast<GenericIPSpeakerImp>(strong_listener)) {
                    auto stats_imp = speakerImp->getSpeakerStatisticImp();
                    if (stats_imp) {
                        auto params = stats_imp->getParams();
                        auto option = params.option;
                        Json::Value item;
                        item["deviceId"] = params.tuple.device_id;
                        // Currently, the speaker online status is determined based on the ONVIF connection status.
                        item["status"] = params.connect;
                        item["errMsg"] = params.status;
                        // Get controller status
                        if (!option.manufacturer.empty()) {
                            item["controller"]["status"] = params.connect;
                            item["controller"]["errMsg"] = params.status;
                        } else {
                            item["controller"] = Json::nullValue;
                        }
                        data.append(item);
                    }
                }
            }
        } catch (std::exception &ex) {
            WarnL << "Report server statistic exception: " << ex.what();
        } catch (...) {
            WarnL << "Report server statistic unknown exception";
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
        for (const auto &vo : profile.vEncoderOptionMap) {
            profileJson["videoEncoder"]["available"][vo.first]["fps"]["supported"] = vo.second.FrameRatesSupported;
            profileJson["videoEncoder"]["available"][vo.first]["fps"]["editable"] = vo.second.FPSEditable;
            profileJson["videoEncoder"]["available"][vo.first]["bitrate"]["supported"] = vo.second.BitRateRange;
            profileJson["videoEncoder"]["available"][vo.first]["bitrate"]["editable"] = vo.second.bitrateEditable;
            profileJson["videoEncoder"]["available"][vo.first]["ResolutionsAvailable"]["supported"] = Json::arrayValue;
            for (const auto &r : vo.second.ResolutionsAvailable) {
                Json::Value stream;
                stream["width"] = r.first;
                stream["height"] = r.second;
                profileJson["videoEncoder"]["available"][vo.first]["ResolutionsAvailable"]["supported"] .append(stream);
            }
            profileJson["videoEncoder"]["available"][vo.first]["ResolutionsAvailable"]["editable"] = vo.second.resolutionEditable;
        }
        profileJson["videoEncEditable"] = profile.videoEncEditable;
        profileJson["videoConfigEditable"] = profile.videoConfigEditable;
        profileJson["hasAudio"] = profile.hasAudio;
        profileJson["acodec"] = profile.acodec;
        profileJson["channelNo"] = profile.channelNo;
        profileJson["sampleBit"] = profile.sampleBit;
        profileJson["sampleRate"] = profile.sampleRate;
        ret.append(profileJson);
    }
    return ret;
}

static Json::Value getVendorFeatureSupportJson(const VendorFeatureSupport &features) {
    Json::Value ret = Json::objectValue;
    ret["requiresSeparateCredential"] = features.requiresSeparateCredential;
    ret["supportsVendorFeatures"] = features.supportsVendorFeatures;

    Json::Value featureArray = Json::arrayValue;
    for (const auto &feature : features.supportedVendorFeatures) {
        featureArray.append(feature);
    }
    ret["supportedVendorFeatures"] = featureArray;

    return ret;
}

Json::Value makeDeviceCapabilitiesJson(const DeviceSource::Ptr &device, const DeviceCapabilities* caps) {
    Json::Value data;
    auto weak_listener = device->getListener();
    if (auto strong_listener = weak_listener.lock()) {
        if (auto cameraImp = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener)) {
            auto stats_imp = cameraImp->getCameraStatisticImp();
            if (stats_imp) {
                auto params = stats_imp->getParams();
                auto option = params.option;
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
                    // note: keep manufacturer and model field if camera is added by user with ip
                    data["manufacturer"] = option.manufacturer.empty() && option.ip.empty() ? GENERIC_RTSP_CAMERA : option.manufacturer;
                    data["model"] = option.model.empty() && option.ip.empty() ? GENERIC_RTSP_CAMERA : option.model;
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
                GET_CONFIG(bool, enableMotion, Motion::kEnableMotion);
                data["motionDetection"]["mediaSupport"] = enableMotion;
#else
                data["motionDetection"]["mediaSupport"] = false;
#endif
                GET_CONFIG(bool, enableAutoProfile, General::kEnableAutoProfile);
                data["enableAutoProfile"] = enableAutoProfile ? true : false;
                // todo: get this value from camera capability instead of global config, because it's possible that some onvif camera doesn't support onvif profile configuration
                data["enableOnvifProfileConfig"] = caps->isOnvifDevice ? true : false;
                data["supportsSdCardPlayback"] = caps->isOnvifDevice ? caps->supportsSdCardPlayback : false;
                data["vendorFeatures"] = getVendorFeatureSupportJson(caps->vendorFeatureSupport);
                GET_CONFIG(bool, enablePrivacymaskSupport, OverlayPrivacyConfig::kEnablePrivacyMask)
                data["enablePrivacyMaskSupport"] = enablePrivacymaskSupport;
                GET_CONFIG(bool, enableWatermarkSupport, OverlayPrivacyConfig::kEnableWatermark)
                data["enableWatermarkSupport"] = enableWatermarkSupport;
                data["liveTransports"] = Json::arrayValue;
                for (const auto &transport : params.transport_stats.liveTransports) {
                    data["liveTransports"].append(transport);
                }
                data["replayTransports"] = Json::arrayValue;
                for (const auto &transport : params.transport_stats.replayTransports) {
                    data["replayTransports"].append(transport);
                }
            }
        }

        if (auto speakerImp = dynamic_pointer_cast<GenericIPSpeakerImp>(strong_listener)) {
            auto option = speakerImp->getSpeakerOption();
            data["deviceId"] = device->getDeviceTuple().device_id;
            data["onvifDevice"] = caps->isOnvifDevice;
            auto deviceInfo = caps->onvifProfile.deviceInfo;
            data["manufacturer"] = deviceInfo.manufacturer;
            data["model"] = deviceInfo.model;
            data["serialNumber"] = deviceInfo.serialNumber;
            data["firmwareVersion"] = deviceInfo.firmwareVersion;
            data["hardwareId"] = deviceInfo.hardwareId;
            data["macAddress"] = deviceInfo.macAddress;
            data["hasWebPage"] = true;
            data["webPage"] = StrPrinter << "http://" << option.ip << ":" << (option.autoWebPort ? option.port : option.webPort);                                
            auto mediaProfiles = caps->onvifProfile.mediaProfiles;
            Json::Value onvifProfileJson = Json::objectValue;
            onvifProfileJson["profiles"] = getOnvifProfileJsonArray(mediaProfiles);
            onvifProfileJson["isPTZ"] = false;
            onvifProfileJson["ptzControlMode"] = Json::arrayValue;
            data["onvifProfiles"] = onvifProfileJson;
            data["motionDetection"]["mediaSupport"] = false;
            data["enableAutoProfile"] = false;
            data["enableOnvifProfileConfig"] = false;
            data["supportsSdCardPlayback"] = false;
            data["enablePrivacyMaskSupport"] = false;
            data["enableWatermarkSupport"] = false;
            data["vendorFeatures"] = getVendorFeatureSupportJson(caps->vendorFeatureSupport);
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

Json::Value makeDeviceStoragesJson() {
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
    val["recordRootPath"]           = option.recordRootPath;
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

    // View overlay policy
    val["enforceWatermarkOnView"] = option.enforceWatermarkOnView;
    val["watermarkTemplateId"] = option.watermarkTemplateId;
    val["watermarkExcludedRoleIds"] = option.watermarkExcludedRoleIds;
    val["watermarkTemplate"] = option.watermarkTemplate;
    val["enforcePrivacyMaskOnView"] = option.enforcePrivacyMaskOnView;
    val["privacyMaskExcludedRoleIds"] = option.privacyMaskExcludedRoleIds;
    val["privacyMaskRegions"] = option.privacyMaskRegions;

    return val;
}

Json::Value makeSpeakerOptionJson(const SpeakerOption &option) {
    Json::Value val;
    // Basic info
    val["name"]         = option.name;
    val["manufacturer"] = option.manufacturer;
    val["model"]        = option.model;
    val["ip"]           = option.ip;
    val["port"]         = option.port;
    val["username"]     = option.username;
    val["password"]     = option.password;

    val["preferedMediaServer"]     = option.preferedMediaServer;
    val["enableActive"]            = option.enableActive;

    // Web access
    val["webPort"]                 = option.webPort;
    val["autoWebPort"]             = option.autoWebPort;

    return val;
}

Json::Value makeDeviceStatisticJson(const DeviceSource::Ptr &device) {
    Json::Value item;
    auto weak_listener = device->getListener();
    if (auto strong_listener = weak_listener.lock()) {
        if (auto cameraImp = std::dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener)) {
            auto camera = cameraImp->getCameraSource();
            auto stats_imp = cameraImp->getCameraStatisticImp();
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

                item["liveTransports"] = Json::arrayValue;
                for (const auto &transport : params.transport_stats.liveTransports) {
                    item["liveTransports"].append(transport);
                }
                item["replayTransports"] = Json::arrayValue;
                for (const auto &transport : params.transport_stats.replayTransports) {
                    item["replayTransports"].append(transport);
                }

                // Get stream reader count
                item["readerAvailableOnMServer"] = GlobalMonitor::Instance().isReaderCountAvailable();
                item["readerAvailablePerCamera"] = GlobalMonitor::Instance().isReaderCountAvailable(params.tuple.device_id);
            }
        }

        if (auto speakerImp = std::dynamic_pointer_cast<GenericIPSpeakerImp>(strong_listener)) {
            auto stats_imp = speakerImp->getSpeakerStatisticImp();
            if (stats_imp) {
                auto params = stats_imp->getParams();
                auto option = params.option;
                item["deviceId"] = params.tuple.device_id;
                // Currently, the speaker online status is determined based on the ONVIF connection status.
                item["status"] = params.connect;
                item["errMsg"] = params.status;
                // Get controller status
                if (!option.manufacturer.empty()) {
                    item["controller"]["status"] = params.connect;
                    item["controller"]["errMsg"] = params.status;
                } else {
                    item["controller"] = Json::nullValue;
                }
                item["options"] = makeSpeakerOptionJson(option);
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

Json::Value makeDeviceMediaProfileJson(const managerkit::DeviceSource::Ptr &device) {
    Json::Value item;
    auto weak_listener = device->getListener();
    if (auto strong_listener = weak_listener.lock()) {
        auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
        if (impl) {
            auto stats_imp = impl->getCameraStatisticImp();
            if (stats_imp) {
                auto params = stats_imp->getParams();
                item["deviceStatus"] = params.device_stats.status;
                item["deviceConnect"] = params.device_stats.connect;
                Json::Value mediaProfiles = Json::arrayValue;
                if (params.device_stats.connect) {
                    for (const auto &mp : params.device_stats.device_caps.onvifProfile.mediaProfiles) {
                        Json::Value mp_json = Json::objectValue;
                        mp_json["token"] = mp.token;
                        mp_json["url"] = mp.url;
                        mp_json["videoEncoder"]["vcodec"] = mp.vcodec;
                        mp_json["videoEncoder"]["width"] = mp.width;
                        mp_json["videoEncoder"]["height"] = mp.height;
                        mp_json["videoEncoder"]["bitrate"] = mp.bitrate;
                        mp_json["videoEncoder"]["fps"] = mp.fps;
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
                        
                        auto it = params.device_stats.stream_settings.find(mp.token);
                        if (it != params.device_stats.stream_settings.end()) {
                            const auto &config = it->second;
                            mp_json["videoEncoder"]["configState"]["retry_time"] = config.state.retry_time;
                            mp_json["videoEncoder"]["configState"]["status"] = config.state.status;
                        } else {
                            mp_json["videoEncoder"]["configState"]["retry_time"] = 5;
                            mp_json["videoEncoder"]["configState"]["status"] = configStateToString[VideoConfigSetState::EXTERNAL];
                        }

                        mediaProfiles.append(mp_json);
                    }
                }
                item["profiles"] = mediaProfiles;
            }
        }
    }
    return item;
}

managerkit::DeviceSource::Ptr findDeviceSource(const std::string &deviceId, const std::string &schema) {
    DeviceTuple tuple;
    tuple.vhost = DEFAULT_VHOST;
    tuple.device_id = deviceId;
    if (!schema.empty()) {
        return DeviceSource::find(schema, tuple.vhost, tuple.device_id);
    }
    return DeviceSource::find(tuple.vhost, tuple.device_id);
};
