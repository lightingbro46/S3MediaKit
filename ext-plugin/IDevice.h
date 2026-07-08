#ifndef S3MEDIAKIT_ISPEAKER_H
#define S3MEDIAKIT_ISPEAKER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "Common/config.h"

namespace mediakit{

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

enum class SupportedFeatures {
    PlayAudioFile,
    // Todo: add more feature
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
using SpeakerConfig = IDeviceConfig;

static DeviceBrand getDeviceBrand(const std::string& manufacturer) {
    if (manufacturer == "Bosch") {
        return DeviceBrand::BOSCH;
    }

    if (manufacturer == "TOA") {
        return DeviceBrand::TOA;
    }

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

    // Get the device type (Speaker, Camera, ...).
    virtual DeviceType getDeviceType() const = 0;

    // Get the device brand.
    virtual DeviceBrand getBrand() const = 0;

    // Get the device configuration.
    virtual IDeviceConfig getConfig() const = 0;
};

}//namespace mediakit

#endif // S3MEDIAKIT_ISPEAKER_H