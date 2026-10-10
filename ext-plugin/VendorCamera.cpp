#include "VendorCamera.h"
#include "VendorHttp.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

// Long enough for a busy camera, short enough that probing every plugin stays within one cycle.
static constexpr float kDeviceInfoTimeoutSec = 5;

VendorCamera::VendorCamera(const IDeviceConfig &cfg) : _cfg(cfg) {}

void VendorCamera::connect(OnDeviceResult cb) {
    const string url = "http://" + _cfg.ip + ":" + to_string(_cfg.port) + deviceInfoPath();
    std::weak_ptr<VendorCamera> weak_self = shared_from_this();
    vendorHttpGet(url, _cfg.username, _cfg.password, kDeviceInfoTimeoutSec, [weak_self, cb](const VendorHttpAnswer &answer) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        DeviceFailure failure = vendorHttpFailure(answer.sockErr, answer.status);
        if (failure != DeviceFailure::None) {
            self->_last_failure = static_cast<int>(failure);
            cb(false, answer.status ? "HTTP " + to_string(answer.status) : string("no answer"));
            return;
        }
        IDeviceInfo info;
        if (!self->parseDeviceInfo(answer.body, info)) {
            self->_last_failure = static_cast<int>(DeviceFailure::Other);
            cb(false, "not this vendor's device information");
            return;
        }
        self->_last_failure = static_cast<int>(DeviceFailure::None);
        {
            std::lock_guard<std::mutex> lock(self->_mtx);
            self->_info = info;
        }
        cb(true, info.model);
    });
}

IDeviceInfo VendorCamera::getDeviceInfo() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _info;
}

} // namespace managerkit
