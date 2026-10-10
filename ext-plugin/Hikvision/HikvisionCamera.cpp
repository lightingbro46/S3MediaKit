#include "HikvisionCamera.h"
#include "Control/PsiaDeviceControl.h"

namespace managerkit {

bool HikvisionCamera::parseIsapiDeviceInfo(const std::string &body, IDeviceInfo &info) {
    OnvifDeviceInfo parsed;
    // deviceName is whatever the user named the camera: never a manufacturer.
    if (!psia_detail::parseDeviceInfo(body, parsed, "Hikvision")) {
        return false;
    }
    info.manufacturer    = parsed.manufacturer;
    info.model           = parsed.model;
    info.firmwareVersion = parsed.firmwareVersion;
    info.serialNumber    = parsed.serialNumber;
    info.hardwareId      = parsed.hardwareId;
    info.macAddress      = parsed.macAddress;
    return true;
}

} // namespace managerkit
