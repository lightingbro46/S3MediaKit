#include "CameraDeviceControl.h"
#include <algorithm>
#include <strings.h>
#include "Control/OnvifDeviceControl.h"
#include "Control/PsiaDeviceControl.h"
#include "Util/logger.h"
#include "Util/util.h"

namespace managerkit {

const char *controlProtocolToString(ControlProtocol protocol) {
    switch (protocol) {
        case ControlProtocol::ONVIF: return "ONVIF";
        case ControlProtocol::PSIA: return "PSIA";
        case ControlProtocol::ISAPI: return "ISAPI";
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
    if (strcasecmp(str.c_str(), "ISAPI") == 0) {
        return ControlProtocol::ISAPI;
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

CameraDeviceControl::Ptr makeDeviceControl(ControlProtocol protocol, const std::string &address,
                                           const std::string &username, const std::string &password) {
    switch (protocol) {
        case ControlProtocol::ONVIF: return std::make_shared<OnvifControl>(address, username, password);
        case ControlProtocol::PSIA: return std::make_shared<PsiaControl>(address, username, password);
        case ControlProtocol::ISAPI: return std::make_shared<IsapiControl>(address, username, password);
        default: return nullptr;
    }
}

std::vector<ControlProtocol> parseProtocolOrder(const std::string &order_str) {
    std::vector<ControlProtocol> order;
    for (auto &name : toolkit::split(order_str, ",")) {
        ControlProtocol protocol = controlProtocolFromString(toolkit::trim(name));
        if (protocol != ControlProtocol::NONE && std::find(order.begin(), order.end(), protocol) == order.end()) {
            order.push_back(protocol);
        }
    }
    if (order.empty()) {
        order.push_back(ControlProtocol::ONVIF);
        order.push_back(ControlProtocol::ISAPI);
        order.push_back(ControlProtocol::PSIA);
    }
    return order;
}

ProtocolProbeResult probeControlProtocols(const std::vector<ControlProtocol> &order,
                                          const DeviceControlFactory &factory, const std::string &log_name,
                                          bool sync_clock_on_skew) {
    ProtocolProbeResult result;
    result.err_msg = "No control protocol answered";
    // Set when ONVIF rejected the credentials: the error reported if no other protocol connects.
    std::string onvif_auth_err;
    DeviceFailure onvif_auth_failure = DeviceFailure::None;
    for (auto protocol : order) {
        auto device_ctr = factory(protocol);
        if (!device_ctr) {
            continue;
        }
        if (device_ctr->connect()) {
            result.winner = device_ctr;
            result.err = ConnectError::NONE;
            result.failure = DeviceFailure::None;
            onvif_auth_err.clear();
            break;
        }
        ConnectError err = device_ctr->lastConnectError();
        result.err = err;
        result.err_msg = std::string(controlProtocolToString(protocol)) + ": " + device_ctr->lastError();
        result.failure = device_ctr->lastFailure();
        if (sync_clock_on_skew && result.failure == DeviceFailure::ClockSkew) {
            device_ctr->setCameraTimeManual();
        }
        InfoL << "Probe " << controlProtocolToString(protocol) << " on device " << log_name << ": "
              << connectErrorToString(err);
        // Destroyed before the next protocol is tried: never two controllers on one device.
        device_ctr.reset();
        if (protocol == ControlProtocol::ONVIF && err == ConnectError::AUTH_FAILED) {
            // A Hikvision camera with ONVIF disabled, or without an ONVIF user, answers 401 to
            // ONVIF while its web account still works for ISAPI.
            onvif_auth_err = result.err_msg;
            onvif_auth_failure = result.failure;
            continue;
        }
        if (err != ConnectError::NOT_THIS_PROTOCOL) {
            // Unreachable, or the password is wrong: that says nothing about the protocol.
            break;
        }
    }
    if (!result.winner && !onvif_auth_err.empty()) {
        result.err_msg = onvif_auth_err;
        result.err = ConnectError::AUTH_FAILED;
        result.failure = onvif_auth_failure;
    }
    return result;
}

bool ReconnectPacer::due(uint64_t now, int interval_sec, int auth_retry_sec) const {
    const int wait_sec = (_auth_failed && auth_retry_sec > interval_sec) ? auth_retry_sec : interval_sec;
    return _last_attempt == 0 || now < _last_attempt || now - _last_attempt >= static_cast<uint64_t>(wait_sec);
}

void ProtocolLock::setSetting(const std::string &setting) {
    _fixed = controlProtocolFromString(setting);
}

void ProtocolLock::restore(ControlProtocol last) {
    _last = last;
    _locked = last != ControlProtocol::NONE;
}

void ProtocolLock::unlock() {
    _locked = false;
    _not_this_protocol_count = 0;
}

ControlProtocol ProtocolLock::fixedOrLocked() const {
    if (_fixed != ControlProtocol::NONE) {
        return _fixed;
    }
    return _locked ? _last : ControlProtocol::NONE;
}

bool ProtocolLock::onConnected(ControlProtocol protocol) {
    bool changed = _last != ControlProtocol::NONE && _last != protocol;
    _last = protocol;
    _locked = true;
    _not_this_protocol_count = 0;
    return changed;
}

bool ProtocolLock::onConnectError(ConnectError err, int reprobe_failures) {
    if (!isAuto()) {
        // A fixed protocol is never swapped for another one, whatever the device answers.
        return false;
    }
    if (err != ConnectError::NOT_THIS_PROTOCOL) {
        _not_this_protocol_count = 0;
        return false;
    }
    return ++_not_this_protocol_count >= std::max(1, reprobe_failures);
}

} // namespace managerkit
