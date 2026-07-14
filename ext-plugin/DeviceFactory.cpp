#include "DeviceFactory.h"
#include <stdexcept>
#include "Bosch/BoschIpSpeaker.h"

namespace managerkit {

IDevice::Ptr DeviceFactory::create(const IDeviceConfig& cfg) {
    switch (cfg.type) {
        case DeviceType::Speaker:
            return createSpeaker(cfg);

        // case DeviceType::Camera:
        //     return createCamera(cfg);

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

} //namespace managerkit