#ifndef S3MEDIAKIT_HANWHACAMERA_H
#define S3MEDIAKIT_HANWHACAMERA_H

#include <map>
#include "../IOnvifSetup.h"
#include "../VendorCamera.h"

namespace managerkit {

// Hanwha Vision (Wisenet, formerly Samsung Techwin) camera plugin: identifies the camera through the
// SUNAPI device information page, and turns ONVIF on when it is off (older firmware). ONVIF logs in
// with the web account, so the plugin creates no user. Control goes through ONVIF.
class HanwhaCamera : public VendorCamera, public IOnvifSetup {
public:
    using VendorCamera::VendorCamera;

    // Turns ONVIF on over SUNAPI when it is off. `username` must be empty: Hanwha has no ONVIF users.
    void prepareOnvif(const std::string &username, const std::string &password, OnDeviceResult cb) override;

    // The SUNAPI deviceinfo answer, as "Key=Value" lines or as JSON → `info`; false without a Model.
    static bool parseSunapiDeviceInfo(const std::string &body, IDeviceInfo &info);
    // A SUNAPI answer ("Key=Value" lines, or a flat JSON object) → its fields; false when it is neither.
    static bool parseSunapiFields(const std::string &body, std::map<std::string, std::string> &fields);

protected:
    const char *deviceInfoPath() const override { return "/stw-cgi/system.cgi?msubmenu=deviceinfo&action=view"; }
    bool parseDeviceInfo(const std::string &body, IDeviceInfo &info) const override {
        return parseSunapiDeviceInfo(body, info);
    }
};

} // namespace managerkit

#endif // S3MEDIAKIT_HANWHACAMERA_H
