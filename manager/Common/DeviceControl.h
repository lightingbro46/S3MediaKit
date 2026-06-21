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

enum class IMAGE_CONTROL_DIRECT {
    FocusAuto,  // Imaging continuous focus auto
    FocusIn,   // Imaging continuous focus near
    FocusOut,  // Imaging continuous focus far
    IrisAuto,  // PTZ Auxiliary iris auto
    IrisIn,    // PTZ Auxiliary iris open
    IrisOut    // PTZ Auxiliary iris close
};


enum class RELAY_OUTPUT_CONTROL {
    RelayOn,   // relay output active
    RelayOff   // relay output inactive
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
