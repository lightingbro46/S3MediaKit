#ifndef SPEAKER_GENERICIPSPEAKER_H
#define SPEAKER_GENERICIPSPEAKER_H

#include "Common/DeviceSource.h"
#include "Common/DeviceOption.h"

namespace managerkit {

class SpeakerOption : public DeviceOption {
public:
    SpeakerOption() {

    }

    // speaker name
    std::string name;

    // Device API username (separate from ONVIF credentials)
    std::string deviceUsername;

    // Device API password (separate from ONVIF credentials)
    std::string devicePassword;

    // Device API port (separate from ONVIF credentials)
    int devicePort = 80;

    // Whether vendor-specific features are enabled
    bool enableVendorFeature = false;

    // List of enabled vendor-specific features
    std::vector<std::string> enabledVendorFeatures;

    // Whether separate device credentials have been configured
    bool separateCredentialConfigured = false;

    // speaker model
    std::string model;

    // media server id, which speaker belong to
    std::string preferedMediaServer;

    // whether to enable speaker
    bool enableActive = false;

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
               deviceUsername == other.deviceUsername &&
               devicePassword == other.devicePassword &&
               devicePort == other.devicePort &&
               enableVendorFeature == other.enableVendorFeature &&
               separateCredentialConfigured == other.separateCredentialConfigured &&
               enabledVendorFeatures == other.enabledVendorFeatures;
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