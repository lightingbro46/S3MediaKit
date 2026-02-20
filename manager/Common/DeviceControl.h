#ifndef COMMON_DEVICECONTROL_H
#define COMMON_DEVICECONTROL_H

#include <string>

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

class DeviceControl {
public:
    using Ptr = std::shared_ptr<DeviceControl>;

    virtual ~DeviceControl() = default;

    virtual bool connect() = 0;

    virtual void disconnect() = 0;
};

} // namespace managerkit

#endif // COMMON_DEVICECONTROL_H
