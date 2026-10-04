#ifndef COMMON_DEVICEFAILURECODE_H
#define COMMON_DEVICEFAILURECODE_H

#include <string>
#include "Common/DeviceControlTypes.h"
#include "Network/Socket.h"
#include "server/WebApiErrCode.h"

namespace managerkit {

// A device command that failed because of the network, the credentials or the device clock reports
// that, so the UI can tell the user what to fix; any other failure keeps the command's own code.
inline ApiErrCode deviceFailureCode(DeviceFailure failure, ApiErrCode fallback) {
    switch (failure) {
        case DeviceFailure::Unauthorized: return ApiErrCode::CODE_DEVICE_UNAUTHORIZED;
        case DeviceFailure::AuthMethodUnsupported: return ApiErrCode::CODE_DEVICE_AUTH_METHOD_NOT_ALLOWED;
        case DeviceFailure::Unreachable: return ApiErrCode::CODE_DEVICE_UNREACHABLE;
        case DeviceFailure::Timeout: return ApiErrCode::CODE_DEVICE_TIMEOUT;
        case DeviceFailure::ClockSkew: return ApiErrCode::CODE_DEVICE_CLOCK_SKEW;
        default: return fallback;
    }
}

// Error for a command that needs the device controller while it is not connected: says why the last
// connect failed (credentials, network, clock) when known, else CODE_DEVICE_OFFLINE.
inline toolkit::SockException deviceNotReadyError(DeviceFailure failure) {
    const char *reason = nullptr;
    switch (failure) {
        case DeviceFailure::Unauthorized: reason = "device rejected the username or password"; break;
        case DeviceFailure::AuthMethodUnsupported: reason = "device only offers HTTP Basic authentication"; break;
        case DeviceFailure::Unreachable: reason = "device unreachable"; break;
        case DeviceFailure::Timeout: reason = "device did not respond"; break;
        case DeviceFailure::ClockSkew: reason = "device clock is out of sync with the server"; break;
        default: break;
    }
    std::string msg = "Device controller is not ready";
    if (reason) {
        msg += std::string(": ") + reason;
    }
    return toolkit::SockException(toolkit::Err_other, msg,
                                  deviceFailureCode(failure, ApiErrCode::CODE_DEVICE_OFFLINE));
}

} // namespace managerkit

#endif // COMMON_DEVICEFAILURECODE_H
