#include "DeviceFactory.h"
#include <stdexcept>
#include "Bosch/BoschIpSpeaker.h"
#include "Hanwha/HanwhaCamera.h"
#include "Hikvision/HikvisionCamera.h"

namespace managerkit {

IDevice::Ptr DeviceFactory::create(const IDeviceConfig& cfg) {
    switch (cfg.type) {
        case DeviceType::Speaker:
            return createSpeaker(cfg);

        case DeviceType::Camera:
            return createCamera(cfg);

        default:
            throw std::runtime_error("Device type is not supported");
    }
}

IDevice::Ptr DeviceFactory::createSpeaker(const IDeviceConfig& cfg) {
    switch (cfg.brand) {
        case DeviceBrand::BOSCH:
            return std::make_shared<BoschIpSpeaker>(cfg);

        default:
            throw std::runtime_error("Speaker brand is not supported");
    }
}

IDevice::Ptr DeviceFactory::createCamera(const IDeviceConfig& cfg) {
    switch (cfg.brand) {
        case DeviceBrand::HIKVISION:
            return std::make_shared<HikvisionCamera>(cfg);

        case DeviceBrand::HANWHA:
            return std::make_shared<HanwhaCamera>(cfg);

        default:
            throw std::runtime_error("Camera brand is not supported");
    }
}

} //namespace managerkit