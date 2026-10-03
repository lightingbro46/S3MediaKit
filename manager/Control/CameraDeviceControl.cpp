#include "CameraDeviceControl.h"
#include <strings.h>

namespace managerkit {

const char *controlProtocolToString(ControlProtocol protocol) {
    switch (protocol) {
        case ControlProtocol::ONVIF: return "ONVIF";
        case ControlProtocol::PSIA: return "PSIA";
        default: return "";
    }
}

ControlProtocol controlProtocolFromString(const std::string &str) {
    if (strcasecmp(str.c_str(), "ONVIF") == 0) {
        return ControlProtocol::ONVIF;
    }
    if (strcasecmp(str.c_str(), "PSIA") == 0) {
        return ControlProtocol::PSIA;
    }
    return ControlProtocol::NONE;
}

const char *connectErrorToString(ConnectError err) {
    switch (err) {
        case ConnectError::NONE: return "NONE";
        case ConnectError::NOT_THIS_PROTOCOL: return "NOT_THIS_PROTOCOL";
        case ConnectError::AUTH_FAILED: return "AUTH_FAILED";
        case ConnectError::UNREACHABLE: return "UNREACHABLE";
        default: return "OTHER";
    }
}

} // namespace managerkit
