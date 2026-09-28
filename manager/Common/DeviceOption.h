#ifndef COMMON_DEVICEINFO_H
#define COMMON_DEVICEINFO_H

#include "Control/OnvifDeviceControl.h"
#include "ext-plugin/IDevice.h"
#include "ext-plugin/IAudioPlayback.h"

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

struct AudioVolumeInfo {
    int minAudioVolume = 0;
    int maxAudioVolume = 0;
    int deviceAudioVolume = 0;
    int configuredAudioVolume = -1; // Not configured yet

    bool operator==(const AudioVolumeInfo &o) const {
        return minAudioVolume         == o.minAudioVolume
            && maxAudioVolume         == o.maxAudioVolume
            && deviceAudioVolume      == o.deviceAudioVolume
            && configuredAudioVolume  == o.configuredAudioVolume;
    }
    bool operator!=(const AudioVolumeInfo &o) const { return !(*this == o); }

    AudioVolumeInfo &operator=(const AudioSettings &o) {
        minAudioVolume      = o.minAudioVolume;
        maxAudioVolume      = o.maxAudioVolume;
        deviceAudioVolume   = o.deviceAudioVolume;
        return *this;
    }

    // AudioVolumeInfo &operator=(const OnvifAudioOutputProfile &o)

    bool supportsControl() const {
        return maxAudioVolume      > minAudioVolume
            && deviceAudioVolume   <= maxAudioVolume
            && deviceAudioVolume   >= minAudioVolume;
    }

    // Convert a normalized volume [0, 100] to the device-specific volume range
    int toDeviceVolume(int volume) const {
        if (maxAudioVolume <= minAudioVolume) {
            return minAudioVolume;
        }

        volume = std::max(0, std::min(100, volume));
        return minAudioVolume + (volume * (maxAudioVolume - minAudioVolume)) / 100;
    }

    // Normalize the device audio volume to the [0, 100] range
    int toNormalizedVolume() const {
        if (maxAudioVolume <= minAudioVolume) {
            return 0;
        }

        int volume = std::max(minAudioVolume, std::min(deviceAudioVolume, maxAudioVolume));
        return (volume - minAudioVolume) * 100 / (maxAudioVolume - minAudioVolume);
    }
};

struct DeviceCapabilities {
    bool isOnvifDevice = false;
    OnvifProfile onvifProfile;
    bool supportsSdCardPlayback = false;
    bool supportsAudioBackchannel = false;
    bool supportsAudioVolumeControl = false;
    AudioVolumeInfo audioVolumeInfo;

    bool operator==(const DeviceCapabilities &o) const {
        return isOnvifDevice == o.isOnvifDevice
            && onvifProfile == o.onvifProfile
            && supportsSdCardPlayback == o.supportsSdCardPlayback
            && supportsAudioBackchannel == o.supportsAudioBackchannel
            && supportsAudioVolumeControl == o.supportsAudioVolumeControl
            && audioVolumeInfo == o.audioVolumeInfo;
    }
    bool operator!=(const DeviceCapabilities &o) const { return !(*this == o); }
};

} // namespace managerkit

#endif // COMMON_DEVICEINFO_H