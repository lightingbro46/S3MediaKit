#ifndef CAMERA_CAMERACONTROL_H
#define CAMERA_CAMERACONTROL_H

#include <string>
#include "Network/Socket.h"

namespace managerkit {

enum class PTZ_DIRECT {
    Up = 1,
    Down,
    Left,
    Right,
    ZoomIn,
    ZoomOut,
    Home
};

class DeviceController {
public:
    using Ptr = std::shared_ptr<DeviceController>;
    
    virtual ~DeviceController() = default;

    virtual bool initControl() { return false; }

    virtual void PTZMove(PTZ_DIRECT direct, int speed, const std::function<void(const toolkit::SockException &ex)> &cb) {
        cb(toolkit::SockException(toolkit::Err_other, "PTZMove not implemented"));
    }
};

} // namespace managerkit

#endif // CAMERA_CAMERACONTROL_H
