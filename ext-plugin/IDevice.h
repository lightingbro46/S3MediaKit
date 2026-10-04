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

static bool requiresSeparateDeviceCredential(DeviceBrand brand) {
    switch (brand) {
    case DeviceBrand::BOSCH:
        return true;

    case DeviceBrand::TOA:
    case DeviceBrand::UNKNOWN:
    default:
        return false;
    }
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