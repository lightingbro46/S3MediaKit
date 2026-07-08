#ifndef S3MEDIAKIT_DEVICEFACTORY_H
#define S3MEDIAKIT_DEVICEFACTORY_H

#include "IDevice.h"

namespace mediakit{

class DeviceFactory {
public:
    static IDevice::Ptr create(const IDeviceConfig& cfg);

private:
    static IDevice::Ptr createSpeaker(const IDeviceConfig& cfg);
    // static IDevice::Ptr createCamera(const IDeviceConfig& cfg);
};

}//namespace mediakit

#endif // S3MEDIAKIT_DEVICEFACTORY_H