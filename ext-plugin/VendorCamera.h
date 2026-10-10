#ifndef S3MEDIAKIT_VENDORCAMERA_H
#define S3MEDIAKIT_VENDORCAMERA_H

#include <atomic>
#include <memory>
#include <mutex>
#include "IDevice.h"

namespace managerkit {

// A camera plugin that only identifies the camera through its vendor API: connect() reads the
// device information page with the configured account (HTTP Digest or Basic). No session, so
// disconnect() has nothing to do. Control stays with the control protocols (ONVIF/ISAPI/PSIA).
class VendorCamera : public IDevice, public std::enable_shared_from_this<VendorCamera> {
public:
    explicit VendorCamera(const IDeviceConfig &cfg);
    ~VendorCamera() override = default;

    // Success when the page answered 200 with this vendor's device information.
    void connect(OnDeviceResult cb) override;
    void disconnect(OnDeviceResult cb) override { cb(true, "no session"); }
    void createOnvifUser(const std::string &username, const std::string &password, OnDeviceResult cb) override {
        cb(false, "Not supported by camera plugins");
    }

    IDeviceInfo getDeviceInfo() const override;
    DeviceType getDeviceType() const override { return DeviceType::Camera; }
    DeviceBrand getBrand() const override { return _cfg.brand; }
    IDeviceConfig getConfig() const override { return _cfg; }
    DeviceFailure lastFailure() const override { return static_cast<DeviceFailure>(_last_failure.load()); }

protected:
    // Path (and query) of the device information page.
    virtual const char *deviceInfoPath() const = 0;
    // The page as this vendor's device information; false when it is not one (another vendor's
    // web server answering the same path).
    virtual bool parseDeviceInfo(const std::string &body, IDeviceInfo &info) const = 0;

private:
    IDeviceConfig _cfg;
    std::atomic<int> _last_failure {static_cast<int>(DeviceFailure::None)};
    mutable std::mutex _mtx;
    IDeviceInfo _info; // guarded by _mtx: written on the HTTP poller
};

} // namespace managerkit

#endif // S3MEDIAKIT_VENDORCAMERA_H
