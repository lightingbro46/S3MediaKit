#ifndef COMMON_DEVICEINFO_H
#define COMMON_DEVICEINFO_H

#include "Control/OnvifDeviceControl.h"
#include "ext-plugin/IDevice.h"

namespace managerkit {

struct DeviceOption {
    std::string manufacturer;
    std::string ip;
    int  port        = 0;
    int  webPort     = 0;
    bool autoWebPort = true;
    std::string username;
    std::string password;
};

struct ControllerOption {
    std::string manufacturer;
    std::string ip;
    int  port        = 0;
    int  webPort     = 0;
    bool autoWebPort = true;
    std::string username;
    std::string password;

    static ControllerOption from(const DeviceOption &o) {
        ControllerOption c;
        c.manufacturer = o.manufacturer;
        c.ip           = o.ip;
        c.port         = o.port;
        c.webPort      = o.webPort;
        c.autoWebPort  = o.autoWebPort;
        c.username     = o.username;
        c.password     = o.password;
        return c;
    }

    bool operator==(const ControllerOption &o) const {
        return manufacturer == o.manufacturer
            && ip           == o.ip
            && port         == o.port
            && webPort      == o.webPort
            && autoWebPort  == o.autoWebPort
            && username     == o.username
            && password     == o.password;
    }
    bool operator!=(const ControllerOption &o) const { return !(*this == o); }
};

struct OnvifProfile {
    std::vector<OnvifMediaProfile> mediaProfiles;
    OnvifPTZProfile ptzProfile;
    OnvifDeviceInfo deviceInfo;
    OnvifImageProfile imageProfile;
    std::vector<OnvifRelayOutputProfile> relayOutputProfiles;
    OnvifAudioOutputProfile audioOutputProfile;
    OnvifAudioInputProfile audioInputProfile;

    bool operator==(const OnvifProfile &o) const {
        return mediaProfiles   == o.mediaProfiles
            && ptzProfile      == o.ptzProfile
            && deviceInfo      == o.deviceInfo
            && imageProfile    == o.imageProfile
            && relayOutputProfiles == o.relayOutputProfiles
            && audioOutputProfile   == o.audioOutputProfile
            && audioInputProfile    == o.audioInputProfile;
    }
    bool operator!=(const OnvifProfile &o) const { return !(*this == o); }
};

struct VendorFeatureSupport {
    bool requiresSeparateCredential = false;
    bool supportsVendorFeatures = false;
    std::vector<std::string> supportedVendorFeatures;

    bool operator==(const VendorFeatureSupport &o) const {
        return requiresSeparateCredential   == o.requiresSeparateCredential
            && supportsVendorFeatures       == o.supportsVendorFeatures
            && supportedVendorFeatures      == o.supportedVendorFeatures;
    }
    bool operator!=(const VendorFeatureSupport &o) const { return !(*this == o); }

    static std::string toString(mediakit::SupportedFeatures feature) {
        switch (feature) {
            case mediakit::SupportedFeatures::PlayAudioFile: return "PlayAudioFile";
            default: return "unknown";
        }
    }
};

struct DeviceCapabilities {
    bool isOnvifDevice = false;
    OnvifProfile onvifProfile;
    bool supportsSdCardPlayback = false;
    VendorFeatureSupport vendorFeatureSupport;

    bool operator==(const DeviceCapabilities &o) const {
        return isOnvifDevice == o.isOnvifDevice
            && onvifProfile == o.onvifProfile
            && supportsSdCardPlayback == o.supportsSdCardPlayback
            && vendorFeatureSupport == o.vendorFeatureSupport;
    }
    bool operator!=(const DeviceCapabilities &o) const { return !(*this == o); }
};

} // namespace managerkit

#endif // COMMON_DEVICEINFO_H