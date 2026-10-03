#include "CameraDeviceControl.h"
#include <algorithm>
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

std::string controlProtocolSetting(const std::string &value) {
    ControlProtocol protocol = controlProtocolFromString(value);
    return protocol == ControlProtocol::NONE ? "AUTO" : controlProtocolToString(protocol);
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

std::string bitrateRangeToString(int minBitrate, int maxBitrate) {
    if (minBitrate <= 0 || maxBitrate < minBitrate)
        return "";

    int steps = (maxBitrate >= 8000) ? 8 : (maxBitrate / 1000);
    steps = std::max(steps, 2);

    double step = static_cast<double>(maxBitrate - minBitrate) / (steps - 1);

    std::string result;

    for (int i = 0; i < steps; ++i) {
        int value;

        if (i == 0) {
            value = minBitrate;
        } else if (i == steps - 1) {
            value = maxBitrate;
        } else {
            int raw = static_cast<int>(minBitrate + i * step);
            int rounded = (raw + 128) / 256 * 256;

            value = std::max(minBitrate, std::min(maxBitrate, rounded));
        }

        if (!result.empty()) {
            size_t pos = result.find_last_of(' ');
            int last = (pos == std::string::npos) ? std::stoi(result) : std::stoi(result.substr(pos + 1));
            if (last == value) continue;

            result += " ";
        }

        result += std::to_string(value);
    }
    return result;
}

} // namespace managerkit
