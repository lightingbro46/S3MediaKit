#ifndef SPEAKER_GENERICIPSPEAKER_H
#define SPEAKER_GENERICIPSPEAKER_H

#include "Common/DeviceSource.h"
#include "Common/DeviceOption.h"
#include "Network/Socket.h"

namespace managerkit {

class SpeakerOption : public DeviceOption {
public:
    SpeakerOption() {

    }

    // speaker name
    std::string name;

    // ONVIF username for device
    std::string onvifUsername;

    // ONVIF password for device
    std::string onvifPassword;

    // ONVIF service port of device
    int onvifPort = 80;

    // speaker model
    std::string model;

    // media server id, which speaker belong to
    std::string preferedMediaServer;

    // whether to enable speaker
    bool enableActive = false;

    int mediaPort = 0;

    bool autoMediaPort = true;

    enum {
        kAudioPlaybackModeAuto = 0,
        kAudioPlaybackModeOnvifBackchannel = 1,
        kAudioPlaybackModeVendorAudioFile = 2
    };
    int audioPlaybackMode = kAudioPlaybackModeAuto;

    bool operator==(const SpeakerOption& other) const{
        return name == other.name &&
               manufacturer == other.manufacturer &&
               model == other.model &&
               ip == other.ip &&
               port == other.port &&
               username == other.username &&
               password == other.password &&
               webPort == other.webPort &&
               autoWebPort == other.autoWebPort &&
               preferedMediaServer == other.preferedMediaServer &&
               enableActive == other.enableActive &&
               mediaPort == other.mediaPort &&
               autoMediaPort == other.autoMediaPort &&
               audioPlaybackMode == other.audioPlaybackMode &&
               controlProtocol == other.controlProtocol;
    }

    bool operator!=(const SpeakerOption& other) const {
        return !(*this == other);
    }
};

class GenericIPSpeakerImp;

class GenericIPSpeaker : public DeviceSource {
public:
    friend class GenericIPSpeakerImp;
    using Ptr = std::shared_ptr<GenericIPSpeaker>;

    GenericIPSpeaker(const DeviceTuple &tuple)
        : DeviceSource(GENERIC_IP_SPEAKER_SCHEMA, tuple) {}

};

} // namespace managerkit

#endif // SPEAKER_GENERICIPSPEAKER_H