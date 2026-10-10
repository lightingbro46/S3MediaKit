#ifndef S3MEDIAKIT_HANWHACAMERA_H
#define S3MEDIAKIT_HANWHACAMERA_H

#include "../VendorCamera.h"

namespace managerkit {

// Hanwha Vision (Wisenet, formerly Samsung Techwin) camera plugin: identifies the camera through the
// SUNAPI device information page. Control goes through ONVIF.
class HanwhaCamera : public VendorCamera {
public:
    using VendorCamera::VendorCamera;

    // The SUNAPI deviceinfo answer, as "Key=Value" lines or as JSON → `info`; false without a Model.
    static bool parseSunapiDeviceInfo(const std::string &body, IDeviceInfo &info);

protected:
    const char *deviceInfoPath() const override { return "/stw-cgi/system.cgi?msubmenu=deviceinfo&action=view"; }
    bool parseDeviceInfo(const std::string &body, IDeviceInfo &info) const override {
        return parseSunapiDeviceInfo(body, info);
    }
};

} // namespace managerkit

#endif // S3MEDIAKIT_HANWHACAMERA_H
