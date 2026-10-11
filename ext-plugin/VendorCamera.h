#ifndef S3MEDIAKIT_VENDORCAMERA_H
#define S3MEDIAKIT_VENDORCAMERA_H

#include <atomic>
#include <memory>
#include <mutex>
#include "IDevice.h"
#include "VendorHttp.h"

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

    IDeviceInfo getDeviceInfo() const override;
    DeviceType getDeviceType() const override { return DeviceType::Camera; }
    DeviceBrand getBrand() const override { return _cfg.brand; }
    IDeviceConfig getConfig() const override { return _cfg; }
    DeviceFailure lastFailure() const override { return static_cast<DeviceFailure>(_last_failure.load()); }

protected:
    // `method` `path` (path and query) on the camera's web server with the configured account. `cb` runs
    // once, on the HTTP poller.
    void request(const std::string &method, const std::string &path, const std::string &body,
                 const std::string &content_type, const OnVendorHttp &cb) const;
    // A step of IOnvifSetup::prepareOnvif() failed: records why and answers `cb`. `account_check`: the
    // first read, where 401 means the configured account is wrong (Unauthorized); anywhere else a
    // 401/403 is a write the account may not make (Other).
    void failPrepare(const VendorHttpAnswer &answer, bool account_check, const std::string &step,
                     const OnDeviceResult &cb);
    void setLastFailure(DeviceFailure failure) { _last_failure = static_cast<int>(failure); }

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
