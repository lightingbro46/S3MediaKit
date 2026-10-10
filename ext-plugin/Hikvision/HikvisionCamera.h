#ifndef S3MEDIAKIT_HIKVISIONCAMERA_H
#define S3MEDIAKIT_HIKVISIONCAMERA_H

#include "../VendorCamera.h"

namespace managerkit {

// Hikvision camera plugin: identifies the camera through GET /ISAPI/System/deviceInfo. Control goes
// through the ISAPI control protocol (IsapiControl) or ONVIF, as configured.
class HikvisionCamera : public VendorCamera {
public:
    using VendorCamera::VendorCamera;

    // An ISAPI DeviceInfo document → `info` (manufacturer "Hikvision" unless it names one).
    static bool parseIsapiDeviceInfo(const std::string &body, IDeviceInfo &info);

protected:
    const char *deviceInfoPath() const override { return "/ISAPI/System/deviceInfo"; }
    bool parseDeviceInfo(const std::string &body, IDeviceInfo &info) const override {
        return parseIsapiDeviceInfo(body, info);
    }
};

} // namespace managerkit

#endif // S3MEDIAKIT_HIKVISIONCAMERA_H
