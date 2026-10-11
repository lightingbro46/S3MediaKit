#ifndef S3MEDIAKIT_IONVIFSETUP_H
#define S3MEDIAKIT_IONVIFSETUP_H

#include <string>
#include "IDevice.h"

namespace managerkit {

// A plugin that makes ONVIF usable through the vendor API (VendorPluginInfo::enablesOnvif or
// ownControlAccount). The controller gets it from the plugin with dynamic_cast, as IAudioPlayback.
class IOnvifSetup {
public:
    virtual ~IOnvifSetup() = default;

    // Turns ONVIF on when it is off; with a `username`, also creates that ONVIF user (or sets its
    // password) with `password`. Reads first and writes only what differs. `cb` runs once, on any
    // thread; on failure lastFailure() says why: Unauthorized only when the device rejected the
    // account itself (not a write the account may not make).
    virtual void prepareOnvif(const std::string &username, const std::string &password, OnDeviceResult cb) = 0;
};

} // namespace managerkit

#endif // S3MEDIAKIT_IONVIFSETUP_H
