#ifndef S3MEDIAKIT_ISPEAKER_H
#define S3MEDIAKIT_ISPEAKER_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "Common/config.h"
#include "Common/DeviceControlTypes.h"

namespace managerkit {

using OnDeviceResult = std::function<void(bool success, const std::string& data)>;

enum class DeviceType {
    Camera,
    Speaker,
};

enum class DeviceBrand {
    UNKNOWN,
    BOSCH,
    TOA,
};

// What a vendor plugin is and what it needs from the control protocol layer (DeviceFactory::plugins()).
struct VendorPluginInfo {
    DeviceType  type = DeviceType::Speaker;
    DeviceBrand brand = DeviceBrand::UNKNOWN;
    // Manufacturer written back when the brand is detected through the plugin's API.
    std::string name;
    // The plugin provisions the account the control protocol logs in with (speaker.onvifUsername)
    // instead of the configured one.
    bool ownControlAccount = false;
    // Control protocol the plugin needs ("ONVIF"); "" = the configured one.
    std::string requiredProtocol;
    // Backchannel audio and the plugin's audio files share the device's DSP.
    bool sharedDsp = false;
};

struct IDeviceInfo {
    std::string manufacturer;
    std::string model;
    std::string firmwareVersion;
    std::string serialNumber;
    std::string hardwareId;
    std::string macAddress;
};

struct IDeviceConfig {
    std::string id;
    std::string name;
    std::string ip;
    int port = 80;
    std::string username;
    std::string password;
    DeviceType  type;
    DeviceBrand brand = DeviceBrand::BOSCH;
};
using SpeakerDeviceInfo = IDeviceInfo;
using SpeakerConfig = IDeviceConfig;

static DeviceBrand getDeviceBrand(const std::string& manufacturer) {
    std::string name = manufacturer;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    if (name.find("bosch") != std::string::npos) {
        return DeviceBrand::BOSCH;
    }

    if (name == "toa") {
        return DeviceBrand::TOA;
    }

    // Unknown vendors are driven over plain ONVIF with the speaker's own credentials.
    return DeviceBrand::UNKNOWN;
}

// No manufacturer configured (blank): the speaker's brand is detected on connect, then written back.
// A non-blank name without a plugin (e.g. "Hikvision") is known: such a device only probes the protocols.
static bool isManufacturerUnset(const std::string& manufacturer) {
    return std::all_of(manufacturer.begin(), manufacturer.end(), [](char c) { return ::isspace(static_cast<unsigned char>(c)) != 0; });
}

class IDevice {
public:
    using Ptr = std::shared_ptr<IDevice>;
    virtual ~IDevice() = default;

    // Establish a connection to the device and create a session.
    virtual void connect(OnDeviceResult cb) = 0;

    // Disconnect from the device and terminate the session.
    virtual void disconnect(OnDeviceResult cb) = 0;

    // Create an ONVIF user on the device.
    virtual void createOnvifUser(const std::string& username, const std::string& password, OnDeviceResult cb) = 0;

    // Get the device information.
    virtual IDeviceInfo getDeviceInfo() const = 0;

    // Get the device type (Speaker, Camera, ...).
    virtual DeviceType getDeviceType() const = 0;

    // Get the device brand.
    virtual DeviceBrand getBrand() const = 0;

    // Get the device configuration.
    virtual IDeviceConfig getConfig() const = 0;

    // Why the last request to the device failed (network, credentials), for the API error codes.
    // Readable from any thread; None when the plugin does not tell.
    virtual DeviceFailure lastFailure() const { return DeviceFailure::None; }
};

} //namespace managerkit

#endif // S3MEDIAKIT_ISPEAKER_H