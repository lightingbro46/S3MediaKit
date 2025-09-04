#ifndef CAMERA_CAMERACONTROL_H
#define CAMERA_CAMERACONTROL_H

#include <string>
#include "Network/Socket.h"

namespace managerkit {

class DeviceController {
public:
    using Ptr = std::shared_ptr<DeviceController>;
    
    virtual ~DeviceController() = default;

    virtual bool initControl() { return false; }
};

} // namespace managerkit

#endif // CAMERA_CAMERACONTROL_H
