#ifndef S3MEDIAKIT_HIKVISIONCAMERA_H
#define S3MEDIAKIT_HIKVISIONCAMERA_H

#include <vector>
#include "../IOnvifSetup.h"
#include "../VendorCamera.h"

namespace managerkit {

// Hikvision camera plugin: identifies the camera through GET /ISAPI/System/deviceInfo, and prepares
// ONVIF over ISAPI (Hikvision keeps ONVIF users apart from the web users, and ONVIF is often off).
// Control goes through the ISAPI control protocol (IsapiControl) or ONVIF, as configured.
class HikvisionCamera : public VendorCamera, public IOnvifSetup {
public:
    using VendorCamera::VendorCamera;

    // Turns ONVIF on (/ISAPI/System/Network/Integrate), then creates the ONVIF user `username`
    // (administrator), or sets its password when it exists (/ISAPI/Security/ONVIF/users).
    void prepareOnvif(const std::string &username, const std::string &password, OnDeviceResult cb) override;

    // An ISAPI DeviceInfo document → `info` (manufacturer "Hikvision" unless it names one).
    static bool parseIsapiDeviceInfo(const std::string &body, IDeviceInfo &info);

    // ONVIF switch of an Integrate document; false when it has none.
    static bool parseIntegrateOnvif(const std::string &xml, bool &enabled);
    // The Integrate document with ONVIF turned on, every other node as the camera sent it.
    static bool enableIntegrateOnvif(const std::string &xml, std::string &out);
    struct OnvifUser {
        std::string id;
        std::string name;
    };
    // UserList of GET /ISAPI/Security/ONVIF/users; false when it is not one.
    static bool parseOnvifUsers(const std::string &xml, std::vector<OnvifUser> &users);
    // The User document that creates or updates the ONVIF administrator `name`.
    static std::string onvifUserXml(const std::string &id, const std::string &name, const std::string &password);

protected:
    const char *deviceInfoPath() const override { return "/ISAPI/System/deviceInfo"; }
    bool parseDeviceInfo(const std::string &body, IDeviceInfo &info) const override {
        return parseIsapiDeviceInfo(body, info);
    }

private:
    void ensureOnvifUser(const std::string &username, const std::string &password, const OnDeviceResult &cb);
};

} // namespace managerkit

#endif // S3MEDIAKIT_HIKVISIONCAMERA_H
