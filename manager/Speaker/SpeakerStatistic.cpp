#include "SpeakerStatistic.h"
#include "Common/config.h"
#include "json/json.h"
#include "Common/StrUtil.h"

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
}

void SpeakerStatisticImp::setDeviceTuple(const DeviceTuple &input_tuple) {
    std::lock_guard<std::mutex> lck(_mtx);
    tuple = input_tuple;
    save();
}

void SpeakerStatisticImp::setSpeakerOption(const SpeakerOption &input_option) {
    std::lock_guard<std::mutex> lck(_mtx);
    if (input_option == option) return;
    option = input_option;
    save();
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
    }
    if (_on_remove && removed) {
        _on_remove(tuple.device_id);
    }
}

void SpeakerStatisticImp::saveOnvifCredentials() {
    if (connect && device_caps.isOnvifDevice) {
        // Same account SpeakerController::setupDeviceController() logs in with.
        bool vendor_account = requiresSeparateDeviceCredential(getDeviceBrand(option.manufacturer));
        GET_CONFIG(std::string, onvif_username, mediakit::Speaker::kOnvifUsername);
        option.onvifUsername = vendor_account ? onvif_username : option.username;
        option.onvifPassword = option.password;
        option.onvifPort = option.port;
    }
}

} // namespace managerkit
