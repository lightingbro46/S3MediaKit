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
    // deviceInfo
    Json::Value deviceInfo = Json::objectValue;
    deviceInfo["manufacturer"] = caps.onvifProfile.deviceInfo.manufacturer;
    deviceInfo["model"] = caps.onvifProfile.deviceInfo.model;
    deviceInfo["firmwareVersion"] = caps.onvifProfile.deviceInfo.firmwareVersion;
    deviceInfo["serialNumber"] = caps.onvifProfile.deviceInfo.serialNumber;
    deviceInfo["hardwareId"] = caps.onvifProfile.deviceInfo.hardwareId;
    deviceInfo["macAddress"] = caps.onvifProfile.deviceInfo.macAddress;
    ret["capabilities"]["onvifProfile"]["deviceInfo"] = deviceInfo;
    Json::Value vendorFeatureSupport = Json::objectValue;
    vendorFeatureSupport["requiresSeparateCredential"] = caps.vendorFeatureSupport.requiresSeparateCredential;
    vendorFeatureSupport["supportsVendorFeatures"] = caps.vendorFeatureSupport.supportsVendorFeatures;
    Json::Value featureArray = Json::arrayValue;
    for (const auto &it : caps.vendorFeatureSupport.supportedVendorFeatures) {
        featureArray.append(it);
    }
    vendorFeatureSupport["supportedVendorFeatures"] = featureArray;
    ret["capabilities"]["vendorFeatureSupport"] = vendorFeatureSupport;
    return ret;
}

static DeviceCapabilities getDeviceCapabilities(const Json::Value &data) {
    DeviceCapabilities stats;
    stats.isOnvifDevice = !data["isOnvifDevice"].empty() ? data["isOnvifDevice"].asBool() : false;
    // deviceInfo
    OnvifProfile profile;
    profile.deviceInfo.manufacturer = data["onvifProfile"]["deviceInfo"]["manufacturer"].asString();
    profile.deviceInfo.model = data["onvifProfile"]["deviceInfo"]["model"].asString();
    profile.deviceInfo.firmwareVersion = data["onvifProfile"]["deviceInfo"]["firmwareVersion"].asString();
    profile.deviceInfo.serialNumber = data["onvifProfile"]["deviceInfo"]["serialNumber"].asString();
    profile.deviceInfo.hardwareId = data["onvifProfile"]["deviceInfo"]["hardwareId"].asString();
    profile.deviceInfo.macAddress = data["onvifProfile"]["deviceInfo"]["macAddress"].asString();
    stats.onvifProfile = profile;
    VendorFeatureSupport features;
    features.requiresSeparateCredential = data["vendorFeatureSupport"]["requiresSeparateCredential"].asBool();
    features.supportsVendorFeatures = data["vendorFeatureSupport"]["supportsVendorFeatures"].asBool();
    for (const auto &it : data["vendorFeatureSupport"]["supportedVendorFeatures"]) {
        features.supportedVendorFeatures.push_back(it.asString());
    }
    stats.vendorFeatureSupport = features;
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
    option.manufacturer = ret["manufacturer"].asString();
    option.model = ret["model"].asString();
    option.username = ret["username"].asString();
    option.password = ret["password"].asString();
    option.devicePort = ret["devicePort"].asInt();
    option.deviceUsername = ret["deviceUsername"].asString();
    option.devicePassword = ret["devicePassword"].asString();
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

    for (const auto& file : ret["audioFiles"]) {
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
    root["manufacturer"] = stats.option.manufacturer;
    root["model"] = stats.option.model;
    root["username"] = stats.option.username;
    root["password"] = stats.option.password;
    root["devicePort"] = stats.option.devicePort;
    root["deviceUsername"] = stats.option.deviceUsername;
    root["devicePassword"] = stats.option.devicePassword;
    root["preferedMediaServer"] = stats.option.preferedMediaServer;

    // add params
    Json::Value params = Json::arrayValue;
    Json::Value device_stats_json = makeDeviceStatisticJson(stats.connect, stats.status, stats.device_caps);
    params.append(makeJsonKeyValue("deviceStatistic", StrJsonUtils::writeJsonString(device_stats_json)));
    root["addParams"] = params;

    // created_at/updated_at
    root["created_at"] = stats.created_at;
    root["updated_at"] = stats.updated_at;

    Json::Value audioFiles(Json::arrayValue);
    for (const auto& item : stats.fileUploaded) {
        const auto& ref = item.second;

        Json::Value file;
        file["id"] = ref.id;
        file["remoteId"] = ref.remoteId;
        file["uploaded_at"] = ref.uploaded_at;

        audioFiles.append(file);
    }

    root["audioFiles"] = audioFiles;

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
        device_caps.vendorFeatureSupport = saved_stats.device_caps.vendorFeatureSupport;
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
        device_caps = *input_caps;
    }
    DebugL << "Device " << tuple.shortUrl() << " capabilities: Connected=" << connect << ", Status=" << status
           << ", isOnvifDevice=" << device_caps.isOnvifDevice;
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

bool SpeakerStatisticImp::isFeatureSupported(SupportedFeatures feature) {
    std::lock_guard<std::mutex> lck(_mtx);
    std::string str_feature = VendorFeatureSupport::toString(feature);
    auto supportedVendorFeatures = device_caps.vendorFeatureSupport.supportedVendorFeatures;
    return std::find(supportedVendorFeatures.begin(),
              supportedVendorFeatures.end(),
              str_feature) != supportedVendorFeatures.end();
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

} // namespace managerkit