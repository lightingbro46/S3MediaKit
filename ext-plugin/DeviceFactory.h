#ifndef S3MEDIAKIT_DEVICEFACTORY_H
#define S3MEDIAKIT_DEVICEFACTORY_H

#include <vector>
#include "IDevice.h"

namespace managerkit {

class DeviceFactory {
public:
    static IDevice::Ptr create(const IDeviceConfig& cfg);

    // Plugins of devices of `type`, in the order a device without manufacturer probes them. Inline:
    // code that only reads the registry (SpeakerStatistic) does not link ext-plugin.
    static const std::vector<VendorPluginInfo> &plugins(DeviceType type) {
        static const std::vector<VendorPluginInfo> speaker_plugins = makeSpeakerPlugins();
        static const std::vector<VendorPluginInfo> camera_plugins = makeCameraPlugins();
        return type == DeviceType::Speaker ? speaker_plugins : camera_plugins;
    }

    // The plugin of `brand` for devices of `type`; false when there is none.
    static bool findPlugin(DeviceType type, DeviceBrand brand, VendorPluginInfo &info) {
        for (const auto &plugin : plugins(type)) {
            if (plugin.brand == brand) {
                info = plugin;
                return true;
            }
        }
        return false;
    }

private:
    static std::vector<VendorPluginInfo> makeSpeakerPlugins() {
        std::vector<VendorPluginInfo> ret;
        VendorPluginInfo bosch;
        bosch.type = DeviceType::Speaker;
        bosch.brand = DeviceBrand::BOSCH;
        bosch.name = "Bosch";
        // Its admin account is not an ONVIF one: it creates speaker.onvifUsername, and its backchannel needs ONVIF.
        bosch.ownControlAccount = true;
        bosch.requiredProtocol = "ONVIF";
        bosch.sharedDsp = true;
        ret.push_back(bosch);
        return ret;
    }

    // Camera plugins only identify the camera (manufacturer, model, serial...): no account of their
    // own, no protocol constraint.
    static std::vector<VendorPluginInfo> makeCameraPlugins() {
        std::vector<VendorPluginInfo> ret;
        VendorPluginInfo hikvision;
        hikvision.type = DeviceType::Camera;
        hikvision.brand = DeviceBrand::HIKVISION;
        hikvision.name = "Hikvision";
        ret.push_back(hikvision);
        VendorPluginInfo hanwha;
        hanwha.type = DeviceType::Camera;
        hanwha.brand = DeviceBrand::HANWHA;
        hanwha.name = "Hanwha Vision";
        ret.push_back(hanwha);
        return ret;
    }

    static IDevice::Ptr createSpeaker(const IDeviceConfig& cfg);
    static IDevice::Ptr createCamera(const IDeviceConfig& cfg);
};

}//namespace managerkit

#endif // S3MEDIAKIT_DEVICEFACTORY_H