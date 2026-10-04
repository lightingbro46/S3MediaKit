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

// Why the last request to a device failed, as far as the user can act on it: fix the network,
// fix the credentials, or nothing specific (the command's own error code applies).
enum class DeviceFailure {
    None = 0,
    Unreachable,           // no connection: refused, no route, connect timeout
    Timeout,               // connected, but the device did not answer in time or dropped the connection
    Unauthorized,          // the device rejected the username or password
    AuthMethodUnsupported, // the device only offers HTTP Basic, which onvif.allowHttpBasicAuth refuses
    Other
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
