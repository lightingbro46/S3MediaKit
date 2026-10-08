#include "SpeakerStatistic.h"
#include "Common/config.h"
#include "json/json.h"
#include "Common/StrUtil.h"
#include "Common/BlockingExecutors.h"
#include "Extension/Resource.h"
#include "ext-plugin/DeviceFactory.h"

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

static Json::Value makeDeviceStatisticJson(const bool &connect, const string &status, const DeviceCapabilities &caps) {
    Json::Value ret = Json::objectValue;
    ret["connect"] = connect;
    ret["status"] = status;
    ret["capabilities"]["isOnvifDevice"] = caps.isOnvifDevice;
    ret["capabilities"]["controlProtocol"] = caps.controlProtocol;
    // deviceInfo
    Json::Value deviceInfo = Json::objectValue;
    deviceInfo["manufacturer"]      = caps.onvifProfile.deviceInfo.manufacturer;
    deviceInfo["model"]             = caps.onvifProfile.deviceInfo.model;
    deviceInfo["firmwareVersion"]   = caps.onvifProfile.deviceInfo.firmwareVersion;
    deviceInfo["serialNumber"]      = caps.onvifProfile.deviceInfo.serialNumber;
    deviceInfo["hardwareId"]        = caps.onvifProfile.deviceInfo.hardwareId;
    deviceInfo["macAddress"]        = caps.onvifProfile.deviceInfo.macAddress;
    ret["capabilities"]["onvifProfile"]["deviceInfo"] = deviceInfo;
    Json::Value audioOutput = Json::objectValue;
    audioOutput["enable"]       = caps.onvifProfile.audioOutputProfile.enable;
    audioOutput["token"]        = caps.onvifProfile.audioOutputProfile.token;
    audioOutput["name"]         = caps.onvifProfile.audioOutputProfile.name;
    audioOutput["outputToken"]  = caps.onvifProfile.audioOutputProfile.outputToken;
    audioOutput["profileToken"] = caps.onvifProfile.audioOutputProfile.profileToken;
    audioOutput["outputLevel"]  = caps.onvifProfile.audioOutputProfile.outputLevel;
    audioOutput["sendPrimacy"]  = caps.onvifProfile.audioOutputProfile.sendPrimacy;
    audioOutput["decoderToken"] = caps.onvifProfile.audioOutputProfile.decoderToken;
    Json::Value supportedArray  = Json::arrayValue;
    for (const auto &it : caps.onvifProfile.audioOutputProfile.supportedDecodingEncodings) {
        supportedArray.append(it);
    }
    audioOutput["supportedDecodingEncodings"] = supportedArray;
    ret["capabilities"]["onvifProfile"]["audioOutputProfile"] = audioOutput;
    Json::Value audioInput = Json::objectValue;
    audioInput["enable"]        = caps.onvifProfile.audioInputProfile.enable;
    audioInput["token"]         = caps.onvifProfile.audioInputProfile.token;
    audioInput["name"]          = caps.onvifProfile.audioInputProfile.name;
    audioInput["sourceToken"]   = caps.onvifProfile.audioInputProfile.sourceToken;
    audioInput["encoderToken"]  = caps.onvifProfile.audioInputProfile.encoderToken;
    audioInput["acodec"]        = caps.onvifProfile.audioInputProfile.acodec;
    audioInput["channelNo"]     = caps.onvifProfile.audioInputProfile.channelNo;
    audioInput["sampleRate"]    = caps.onvifProfile.audioInputProfile.sampleRate;
    audioInput["sampleBit"]     = caps.onvifProfile.audioInputProfile.sampleBit;
    ret["capabilities"]["onvifProfile"]["audioInputProfile"] = audioInput;
    ret["capabilities"]["supportsAudioVolumeControl"] = caps.supportsAudioVolumeControl;
    ret["capabilities"]["audioVolumeInfo"]["minAudioVolume"] = caps.audioVolumeInfo.minAudioVolume;
    ret["capabilities"]["audioVolumeInfo"]["maxAudioVolume"] = caps.audioVolumeInfo.maxAudioVolume;
    ret["capabilities"]["audioVolumeInfo"]["deviceAudioVolume"] = caps.audioVolumeInfo.deviceAudioVolume;
    ret["capabilities"]["audioVolumeInfo"]["configuredAudioVolume"] = caps.audioVolumeInfo.configuredAudioVolume;
    ret["capabilities"]["supportsAudioBackchannel"] = caps.supportsAudioBackchannel;
    ret["capabilities"]["vendorApiConnected"] = caps.vendorApiConnected;
    ret["capabilities"]["vendorApiStatus"] = caps.vendorApiStatus;
    ret["capabilities"]["supportsAudioFilePlayback"] = caps.supportsAudioFilePlayback;
    return ret;
}

static DeviceCapabilities getDeviceCapabilities(const Json::Value &data) {
    DeviceCapabilities stats;
    stats.isOnvifDevice = !data["isOnvifDevice"].empty() ? data["isOnvifDevice"].asBool() : false;
    stats.controlProtocol = data["controlProtocol"].isString() ? data["controlProtocol"].asString()
                          : (stats.isOnvifDevice ? "ONVIF" : "");
    // deviceInfo
    OnvifProfile profile;
    profile.deviceInfo.manufacturer     = data["onvifProfile"]["deviceInfo"]["manufacturer"].asString();
    profile.deviceInfo.model            = data["onvifProfile"]["deviceInfo"]["model"].asString();
    profile.deviceInfo.firmwareVersion  = data["onvifProfile"]["deviceInfo"]["firmwareVersion"].asString();
    profile.deviceInfo.serialNumber     = data["onvifProfile"]["deviceInfo"]["serialNumber"].asString();
    profile.deviceInfo.hardwareId       = data["onvifProfile"]["deviceInfo"]["hardwareId"].asString();
    profile.deviceInfo.macAddress       = data["onvifProfile"]["deviceInfo"]["macAddress"].asString();
    // audio output profile
    if (!data["onvifProfile"]["audioOutputProfile"].isNull() && data["onvifProfile"]["audioOutputProfile"].isObject()) {
        profile.audioOutputProfile.enable       = data["onvifProfile"]["audioOutputProfile"]["enable"].asBool();
        profile.audioOutputProfile.token        = data["onvifProfile"]["audioOutputProfile"]["token"].asString();
        profile.audioOutputProfile.name         = data["onvifProfile"]["audioOutputProfile"]["name"].asString();
        profile.audioOutputProfile.outputToken  = data["onvifProfile"]["audioOutputProfile"]["outputToken"].asString();
        profile.audioOutputProfile.profileToken = data["onvifProfile"]["audioOutputProfile"]["profileToken"].asString();
        profile.audioOutputProfile.outputLevel  = data["onvifProfile"]["audioOutputProfile"]["outputLevel"].asInt();
        profile.audioOutputProfile.sendPrimacy  = data["onvifProfile"]["audioOutputProfile"]["sendPrimacy"].asString();
        profile.audioOutputProfile.decoderToken = data["onvifProfile"]["audioOutputProfile"]["decoderToken"].asString();
        for (const auto &it : data["onvifProfile"]["audioOutputProfile"]["supportedDecodingEncodings"]) {
            profile.audioOutputProfile.supportedDecodingEncodings.push_back(it.asString());
        }
    }
    // audio input profile
    if (!data["onvifProfile"]["audioInputProfile"].isNull() && data["onvifProfile"]["audioInputProfile"].isObject()) {
        profile.audioInputProfile.enable        = data["onvifProfile"]["audioInputProfile"]["enable"].asBool();
        profile.audioInputProfile.token         = data["onvifProfile"]["audioInputProfile"]["token"].asString();
        profile.audioInputProfile.name          = data["onvifProfile"]["audioInputProfile"]["name"].asString();
        profile.audioInputProfile.sourceToken   = data["onvifProfile"]["audioInputProfile"]["sourceToken"].asString();
        profile.audioInputProfile.encoderToken  = data["onvifProfile"]["audioInputProfile"]["encoderToken"].asString();
        profile.audioInputProfile.acodec        = data["onvifProfile"]["audioInputProfile"]["acodec"].asString();
        profile.audioInputProfile.channelNo     = data["onvifProfile"]["audioInputProfile"]["channelNo"].asInt();
        profile.audioInputProfile.sampleRate    = data["onvifProfile"]["audioInputProfile"]["sampleRate"].asString();
        profile.audioInputProfile.sampleBit     = data["onvifProfile"]["audioInputProfile"]["sampleBit"].asString();
    }
    stats.onvifProfile = profile;
    stats.supportsAudioVolumeControl = !data["supportsAudioVolumeControl"].empty() ? data["supportsAudioVolumeControl"].asBool() : false;
    if (!data["audioVolumeInfo"].isNull() && data["audioVolumeInfo"].isObject()) {
        stats.audioVolumeInfo.minAudioVolume            = data["audioVolumeInfo"]["minAudioVolume"].asInt();
        stats.audioVolumeInfo.maxAudioVolume            = data["audioVolumeInfo"]["maxAudioVolume"].asInt();
        stats.audioVolumeInfo.deviceAudioVolume        = data["audioVolumeInfo"]["deviceAudioVolume"].asInt();
        stats.audioVolumeInfo.configuredAudioVolume    = data["audioVolumeInfo"]["configuredAudioVolume"].asInt();
    }
    stats.supportsAudioBackchannel = !data["supportsAudioBackchannel"].empty() ? data["supportsAudioBackchannel"].asBool() : false;
    stats.vendorApiConnected = data["vendorApiConnected"].isBool() ? data["vendorApiConnected"].asBool() : false;
    stats.vendorApiStatus = data["vendorApiStatus"].isString() ? data["vendorApiStatus"].asString() : "";
    stats.supportsAudioFilePlayback = data["supportsAudioFilePlayback"].isBool() ? data["supportsAudioFilePlayback"].asBool() : false;
    return stats;
}

// ################### SpeakerStatisticHelper ###########################

bool SpeakerStatisticHelper::getParams(const string &json_str, SpeakerStatistic &stats) {
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

    SpeakerOption option;
    option.name = ret["name"].asString();
    option.ip = ret["ip"].asString();
    option.port = ret["port"].asInt();
    option.webPort = ret["webPort"].asInt();
    option.autoWebPort = ret["autoWebPort"].asBool();
    option.manufacturer = ret["manufacturer"].asString();
    option.model = ret["model"].asString();
    option.username = ret["username"].asString();
    option.password = ret["password"].asString();
    option.onvifPort = ret["onvifPort"].asInt();
    option.onvifUsername = ret["onvifUsername"].asString();
    option.onvifPassword = ret["onvifPassword"].asString();
    option.preferedMediaServer = ret["preferedMediaServer"].asString();
    option.enableActive = ret["enableActive"].asBool();
    option.mediaPort = ret["mediaPort"].asInt();
    option.autoMediaPort = ret["autoMediaPort"].asBool();
    option.audioPlaybackMode = ret["audioPlaybackMode"].asInt();
    option.controlProtocol = controlProtocolSetting(ret["controlProtocol"].asString());
    option.enableTimeSync = ret.isMember("enableTimeSync") ? ret["enableTimeSync"].asBool() : true;
    stats.option = option;

    stats.connect = ret["connect"].asBool();
    stats.status = ret["status"].asString();

    // params
    for (const auto &it : ret["addParams"]) {
        if (it["name"] == "deviceStatistic") {
            Json::Value device_stats_json;
            StrJsonUtils::readJsonString(it["value"].asString(), device_stats_json);
            if (!device_stats_json["capabilities"].empty()) {
                stats.device_caps = getDeviceCapabilities(device_stats_json["capabilities"]);
            }
            stats.connect = !device_stats_json["connect"].empty() ? device_stats_json["connect"].asBool() : false;
            stats.status = !device_stats_json["status"].empty() ? device_stats_json["status"].asString() : "";
        } 
    }

    // created_at/updated_at
    stats.created_at = ret["created_at"].asUInt64();
    stats.updated_at= ret["updated_at"].asUInt64();

    // Files saved before the key was renamed are stored under "audioFiles".
    const Json::Value &uploaded = ret.isMember("audioFileUploaded") ? ret["audioFileUploaded"] : ret["audioFiles"];
    for (const auto& file : uploaded) {
        SpeakerAudioRef ref;

        ref.id = file["id"].asString();
        ref.remoteId = file["remoteId"].asString();
        ref.uploaded_at = file["uploaded_at"].asUInt64();

        if (!ref.id.empty()) {
            stats.fileUploaded[ref.id] = std::move(ref);
        }
    }

    return true;
}

string SpeakerStatisticHelper::getParamsString(const SpeakerStatistic &stats) {
    Json::Value root;
    // Speaker info
    root["vhost"] = stats.tuple.vhost;
    root["id"] = stats.tuple.device_id;
    
    // Speaker option
    root["name"] = stats.option.name;
    root["ip"] = stats.option.ip;
    root["port"] = stats.option.port;
    root["webPort"] = stats.option.webPort;
    root["autoWebPort"] = stats.option.autoWebPort;
    root["manufacturer"] = stats.option.manufacturer;
    root["model"] = stats.option.model;
    root["username"] = stats.option.username;
    root["password"] = stats.option.password;
    root["onvifPort"] = stats.option.onvifPort;
    root["onvifUsername"] = stats.option.onvifUsername;
    root["onvifPassword"] = stats.option.onvifPassword;
    root["preferedMediaServer"] = stats.option.preferedMediaServer;
    root["enableActive"] = stats.option.enableActive;
    root["mediaPort"] = stats.option.mediaPort;
    root["autoMediaPort"] = stats.option.autoMediaPort;
    root["audioPlaybackMode"] = stats.option.audioPlaybackMode;
    root["controlProtocol"] = stats.option.controlProtocol;
    root["enableTimeSync"] = stats.option.enableTimeSync;

    // add params
    Json::Value params = Json::arrayValue;
    Json::Value device_stats_json = makeDeviceStatisticJson(stats.connect, stats.status, stats.device_caps);
    params.append(makeJsonKeyValue("deviceStatistic", StrJsonUtils::writeJsonString(device_stats_json)));
    root["addParams"] = params;

    // created_at/updated_at
    root["created_at"] = stats.created_at;
    root["updated_at"] = stats.updated_at;

    Json::Value audioFileUploaded(Json::arrayValue);
    for (const auto& item : stats.fileUploaded) {
        const auto& ref = item.second;

        Json::Value file;
        file["id"] = ref.id;
        file["remoteId"] = ref.remoteId;
        file["uploaded_at"] = ref.uploaded_at;

        audioFileUploaded.append(file);
    }

    root["audioFileUploaded"] = audioFileUploaded;

    return root.toStyledString();
}

// ################### SpeakerStatisticImp ###########################

SpeakerStatisticImp::SpeakerStatisticImp(const std::string &src_path) {
    CHECK(!src_path.empty(), "Source path cannot be empty");
    // Same as CameraStatisticImp: one serial queue per speaker keeps its ESC writes in order.
    _esc_queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::api(), "esc " + src_path);
    setup(src_path);
}

SpeakerStatisticImp::~SpeakerStatisticImp() {}

void SpeakerStatisticImp::setup(const string &src_path) {
    auto file_path = src_path;
    if (!end_with(file_path, "/info.txt")) {
        file_path += "/info.txt";
    }
    _file = std::make_shared<FileRecorder<SpeakerStatistic, SpeakerStatisticHelper>>(file_path);
    if (!_file->empty()) {
        load();
    }
}

void SpeakerStatisticImp::load() {
    SpeakerStatistic saved_stats;
    if (_file->load(saved_stats)) {
        // Manually assign fields from stats to this, include Speaker_info
        tuple = saved_stats.tuple;
        option = saved_stats.option;
        connect = saved_stats.connect;
        status = saved_stats.status;
        created_at = saved_stats.created_at;
        updated_at = saved_stats.updated_at;
        fileUploaded = saved_stats.fileUploaded;
        // The volume the user configured is only known here: the device reports its own level.
        device_caps.audioVolumeInfo.configuredAudioVolume = saved_stats.device_caps.audioVolumeInfo.configuredAudioVolume;
        // The protocol AUTO locked to, restored into the controller by GenericIPSpeakerImp.
        device_caps.controlProtocol = saved_stats.device_caps.controlProtocol;
        device_caps.isOnvifDevice = saved_stats.device_caps.isOnvifDevice;
    }
}

void SpeakerStatisticImp::save() {
    if (created_at == 0) {
        created_at = time(nullptr);
    }
    updated_at = time(nullptr);
    _file->save(static_cast<const SpeakerStatistic &>(*this));
    // Retry a config sync that was not written yet (sync was off or the write failed).
    if (_esc_dirty) {
        syncToEsc(false);
    }
}

void SpeakerStatisticImp::markEscConfigChanged() {
    _esc_dirty = true;
    syncToEsc(true);
}

void SpeakerStatisticImp::setDeviceTuple(const DeviceTuple &input_tuple) {
    std::lock_guard<std::mutex> lck(_mtx);
    tuple = input_tuple;
    save();
    markEscConfigChanged();
}

void SpeakerStatisticImp::setSpeakerOption(const SpeakerOption &input_option) {
    std::lock_guard<std::mutex> lck(_mtx);
    if (input_option == option) return;
    option = input_option;
    save();
    markEscConfigChanged();
}

void SpeakerStatisticImp::addDeviceCapabilities(bool input_connect, string input_status, const DeviceCapabilities *input_caps) {
    std::lock_guard<std::mutex> lck(_mtx);
    connect = input_connect;
    status = input_status;
    if (input_caps) {
        // The controller does not know the configured volume: keep the one set through setAudioVolume().
        int configured_volume = device_caps.audioVolumeInfo.configuredAudioVolume;
        device_caps = *input_caps;
        device_caps.audioVolumeInfo.configuredAudioVolume = configured_volume;
    }
    saveOnvifCredentials();
    DebugL << "Device " << tuple.shortUrl() << " capabilities: Connected=" << connect << ", Status=" << status
           << ", isOnvifDevice=" << device_caps.isOnvifDevice << ", controlProtocol=" << device_caps.controlProtocol;
    save();
}

std::string SpeakerStatisticImp::getAudioRemoteId(const std::string &fileId) {
    auto it = fileUploaded.find(fileId);
    if (it == fileUploaded.end()) {
        return "";
    }

    return it->second.remoteId;
}

void SpeakerStatisticImp::setAudioRemoteId(const std::string &fileId, const std::string &remoteId) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = fileUploaded.find(fileId);

    if (it != fileUploaded.end()) {
        it->second.remoteId = remoteId;
        it->second.uploaded_at = time(nullptr);
    } else {
        SpeakerAudioRef ref;
        ref.id = fileId;
        ref.remoteId = remoteId;
        ref.uploaded_at = time(nullptr);

        fileUploaded[fileId] = std::move(ref);
    }
    save();
}

AudioVolumeInfo SpeakerStatisticImp::getAudioVolumeInfo() {
    std::lock_guard<std::mutex> lck(_mtx);
    return device_caps.audioVolumeInfo;
}

void SpeakerStatisticImp::setAudioVolume(const int deviceVolume, const int configuredVolume) {
    std::lock_guard<std::mutex> lck(_mtx);
    device_caps.audioVolumeInfo.deviceAudioVolume = deviceVolume;
    device_caps.audioVolumeInfo.configuredAudioVolume = configuredVolume;
    save();
}

SpeakerStatistic SpeakerStatisticImp::getParams() {
    std::lock_guard<std::mutex> lck(_mtx);
    return static_cast<const SpeakerStatistic &>(*this);
}

void SpeakerStatisticImp::remove() {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        if (_file) {
            _file->remove();
            DebugL << "Removed file recorder success: " << tuple.shortUrl();
            removed = true;
        }
        // Queued behind any pending ESC writes of this speaker, so they cannot recreate it afterwards.
        removeFromEsc();
        _esc_removed = true;
    }
    if (_on_remove && removed) {
        _on_remove(tuple.device_id);
    }
}

template<>
struct ResourceAdapter<SpeakerStatistic> {
    static VmsResource toVmsResource(const SpeakerStatistic &stats) {
        VmsResource res;
        res.guid = stats.tuple.device_id;
        // parent_guid is the speaker's media server, as for a camera.
        res.parent_guid = stats.option.preferedMediaServer;
        res.name = stats.tuple.name;
        if (!stats.option.ip.empty()) {
            res.url = StrPrinter << "http://" << stats.option.ip << ":" << (stats.option.autoWebPort ? stats.option.port : stats.option.webPort);
        }
        res.xtype_guid = getXtypeId();
        return res;
    }

    static std::vector<VmsKvPair> toKvPairs(const SpeakerStatistic &stats) {
        const std::string &resource_id = stats.tuple.device_id;
        const SpeakerOption &opt = stats.option;
        std::vector<VmsKvPair> kvs;

        auto make = [&resource_id](const std::string &key, const std::string &val) {
            VmsKvPair kv;
            kv.resource_guid = resource_id;
            kv.name  = key;
            kv.value = val;
            return kv;
        };
        // The ONVIF account (onvifUsername/onvifPassword/onvifPort) is derived on connect, not synced.
        kvs.push_back(make("name",                opt.name));
        kvs.push_back(make("ip",                  opt.ip));
        kvs.push_back(make("port",                std::to_string(opt.port)));
        kvs.push_back(make("manufacturer",        opt.manufacturer));
        kvs.push_back(make("model",               opt.model));
        kvs.push_back(make("username",            opt.username));
        kvs.push_back(make("password",            opt.password));
        kvs.push_back(make("webPort",             std::to_string(opt.webPort)));
        kvs.push_back(make("autoWebPort",         std::to_string(opt.autoWebPort)));
        kvs.push_back(make("enableActive",        std::to_string(opt.enableActive)));
        kvs.push_back(make("preferedMediaServer", opt.preferedMediaServer));
        kvs.push_back(make("mediaPort",           std::to_string(opt.mediaPort)));
        kvs.push_back(make("autoMediaPort",       std::to_string(opt.autoMediaPort)));
        kvs.push_back(make("audioPlaybackMode",   std::to_string(opt.audioPlaybackMode)));
        kvs.push_back(make("controlProtocol",     opt.controlProtocol));
        kvs.push_back(make("enableTimeSync",      std::to_string(opt.enableTimeSync)));
        return kvs;
    }

    static std::vector<LocalResource> toLocalProps(const SpeakerStatistic &stats) {
        // No local property for speaker statistic
        return std::vector<LocalResource>();
    }

    static SpeakerStatistic fromVmsResource(const VmsResource &res, const std::vector<VmsKvPair> &kvs, const std::vector<LocalResource> &props) {
        SpeakerStatistic stats;
        stats.tuple.vhost = DEFAULT_VHOST;
        stats.tuple.device_id = res.guid;
        stats.tuple.name = res.name;
        stats.option.preferedMediaServer = res.parent_guid;

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
                } else if (kv.name == "enableActive") {
                    stats.option.enableActive = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "preferedMediaServer") {
                    stats.option.preferedMediaServer = kv.value;
                } else if (kv.name == "mediaPort") {
                    stats.option.mediaPort = std::stoi(kv.value);
                } else if (kv.name == "autoMediaPort") {
                    stats.option.autoMediaPort = static_cast<bool>(std::stoi(kv.value));
                } else if (kv.name == "audioPlaybackMode") {
                    stats.option.audioPlaybackMode = std::stoi(kv.value);
                } else if (kv.name == "controlProtocol") {
                    stats.option.controlProtocol = controlProtocolSetting(kv.value);
                } else if (kv.name == "enableTimeSync") {
                    stats.option.enableTimeSync = static_cast<bool>(std::stoi(kv.value));
                }
            } catch (const std::exception &e) {
                WarnL << "Failed to parse KV pair '" << kv.name << "' = '" << kv.value << "': " << e.what();
            }
        }
        return stats;
    }

    static std::string getXtypeId() {
        auto xtypeId = ResourceTypeManager::Instance().getResourceTypeGuid(RESOURCE_TYPE_SPEAKER);
        CHECK(!xtypeId.empty(), "XtypeId for Speaker cannot be empty");
        return xtypeId;
    }
};

bool SpeakerStatisticImp::syncToEsc(bool force) {
    if (!_sync_mode || _esc_removed || !_esc_dirty) {
        return false;
    }
    // Off: the config stays dirty, so the next change or save() publishes it once turned back on.
    GET_CONFIG(bool, sync_config_to_db, mediakit::Speaker::kSyncConfigToDb);
    if (!sync_config_to_db) {
        return false;
    }
    auto sync_time = static_cast<uint64_t>(time(nullptr));
    if (!force && sync_time - _last_sync_time < static_cast<uint64_t>(_sync_interval_sec)) {
        TraceL << "Sync to ESC retry skipped for speaker " << tuple.shortUrl() << " since last sync was at " << getTimeStr("%Y-%m-%d %H:%M:%S", _last_sync_time);
        return false;
    }
    _last_sync_time = sync_time;
    _esc_dirty = false;

    SpeakerStatistic params = static_cast<const SpeakerStatistic &>(*this);
    std::weak_ptr<SpeakerStatisticImp> weak_self = shared_from_this();
    _esc_queue->post([weak_self, params]() {
        try {
            ResourceManager::Instance().addResource<SpeakerStatistic>(params, true);
            TraceL << "Sync to ESC for speaker " << params.tuple.shortUrl();
        } catch (const std::exception &e) {
            ErrorL << "Failed to sync speaker resource to ESC for speaker " << params.tuple.shortUrl() << ": " << e.what();
            auto self = weak_self.lock();
            if (self) {
                std::lock_guard<std::mutex> lck(self->_mtx);
                self->_esc_dirty = true; // retried by the next save()
            }
        }
    });
    return true;
}

void SpeakerStatisticImp::removeFromEsc() {
    GET_CONFIG(bool, sync_config_to_db, mediakit::Speaker::kSyncConfigToDb);
    if (!sync_config_to_db || _esc_removed) {
        return;
    }
    auto device_id = tuple.device_id;
    auto device_url = tuple.shortUrl();
    _esc_queue->post([device_id, device_url]() {
        try {
            auto &resources = ResourceManager::Instance();
            SpeakerStatistic stats;
            if (!resources.getResource<SpeakerStatistic>(device_id, stats)) {
                return;
            }
            // The speaker was moved, not deleted: the record belongs to the media server that runs it now.
            auto owner = stats.option.preferedMediaServer;
            if (!owner.empty() && owner != resources.getSelfNodeId()) {
                DebugL << "Speaker " << device_url << " belongs to media server " << owner << ". Keep its ESC resource";
                return;
            }
            resources.removeResource(device_id);
            TraceL << "Removed speaker resource from ESC for speaker " << device_url;
        } catch (const std::exception &e) {
            ErrorL << "Failed to remove speaker resource from ESC for speaker " << device_url << ": " << e.what();
        }
    });
}

void SpeakerStatisticImp::setSyncMode(bool enable) {
    std::lock_guard<std::mutex> lck(_mtx);
    const bool was_enabled = _sync_mode;
    _sync_mode = enable;
    DebugL << "Set sync mode for speaker " << tuple.shortUrl() << " to " << (_sync_mode ? "enabled" : "disabled");
    if (enable && !was_enabled) {
        // The option may be unchanged since startup (loaded from info.txt), so publish it now.
        markEscConfigChanged();
    } else if (enable) {
        // Called again on every server config reload: refresh ESC at most every _sync_interval_sec.
        // A no-op when ESC already matches; restores the record when the node that ran the speaker
        // before deleted it from its stale view of a move (its delete can replicate after our write).
        _esc_dirty = true;
        syncToEsc(false);
    }
}

void SpeakerStatisticImp::saveOnvifCredentials() {
    if (connect && device_caps.isOnvifDevice) {
        // Same account SpeakerController::setupDeviceController() logs in with.
        VendorPluginInfo plugin;
        bool vendor_account = DeviceFactory::findPlugin(DeviceType::Speaker, getDeviceBrand(option.manufacturer), plugin) &&
                              plugin.ownControlAccount;
        GET_CONFIG(std::string, onvif_username, mediakit::Speaker::kOnvifUsername);
        option.onvifUsername = vendor_account ? onvif_username : option.username;
        option.onvifPassword = option.password;
        option.onvifPort = option.port;
    }
}

} // namespace managerkit
