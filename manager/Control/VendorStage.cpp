#include "VendorStage.h"
#include <algorithm>
#include "ext-plugin/DeviceFactory.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

void VendorStage::configure(DeviceType type, const std::string &manufacturer) {
    _type = type;
    _name.clear();
    _cycle_answered = false;
    _unreachable_cycles = 0;
    setBrand(getDeviceBrand(manufacturer), "");
    // Nothing to detect with: a type without plugins keeps the behavior of a generic device.
    _detecting = isManufacturerUnset(manufacturer) && !DeviceFactory::plugins(type).empty();
}

void VendorStage::setBrand(DeviceBrand brand, const std::string &name) {
    _brand = brand;
    _name = name;
    _has_plugin = DeviceFactory::findPlugin(_type, brand, _plugin);
}

bool VendorStage::onVendorProbe(const ProbeResult &result) {
    if (result.brand == DeviceBrand::UNKNOWN) {
        _cycle_answered = result.answered;
        return false;
    }
    _detecting = false;
    _unreachable_cycles = 0;
    VendorPluginInfo info;
    setBrand(result.brand, DeviceFactory::findPlugin(_type, result.brand, info) ? info.name : "");
    return true;
}

bool VendorStage::identify(const std::string &manufacturer) {
    _detecting = false;
    _unreachable_cycles = 0;
    if (isManufacturerUnset(manufacturer)) {
        return false;
    }
    setBrand(getDeviceBrand(manufacturer), manufacturer);
    return true;
}

void VendorStage::onCycleFailed(bool unreachable) {
    if (unreachable && !_cycle_answered) {
        ++_unreachable_cycles;
    } else {
        _unreachable_cycles = 0;
    }
}

int VendorStage::cycleIntervalSec(int max_interval_sec) const {
    int interval = 60;
    for (int i = 0; i < _unreachable_cycles && interval < max_interval_sec; ++i) {
        interval *= 2;
    }
    return std::max(60, std::min(interval, max_interval_sec));
}

// Logs in to the API of plugins[index] and the ones after it, until one accepts.
static void probePlugin(const std::vector<VendorPluginInfo> &plugins, size_t index, const IDeviceConfig &cfg,
                        VendorStage::ProbeResult result, const VendorStage::OnProbe &cb) {
    if (index >= plugins.size()) {
        cb(result);
        return;
    }
    IDeviceConfig brand_cfg = cfg;
    brand_cfg.brand = plugins[index].brand;
    IDevice::Ptr device;
    try {
        device = DeviceFactory::create(brand_cfg);
    } catch (const std::exception &e) {
        TraceL << "Create device failed brand=" << static_cast<int>(brand_cfg.brand) << " : " << e.what();
        probePlugin(plugins, index + 1, cfg, result, cb);
        return;
    }
    // `device` keeps its pending requests alive and they hold this callback: the callback drops its
    // copy on every path so the device is not kept alive by itself. `plugins` is the static registry.
    device->connect([device, &plugins, index, cfg, result, cb](bool success, const std::string &data) mutable {
        auto found = std::move(device);
        if (success) {
            result.brand = found->getBrand();
            result.answered = true;
            cb(result);
            found->disconnect([](bool, const std::string &) {});
            return;
        }
        auto failure = found->lastFailure();
        TraceL << "Vendor API brand=" << static_cast<int>(found->getBrand()) << " of " << cfg.ip << ":" << cfg.port
               << " refused: " << data;
        if (failure == DeviceFailure::Unauthorized) {
            result.rejected = true;
        }
        if (failure != DeviceFailure::Unreachable && failure != DeviceFailure::Timeout) {
            result.answered = true;
        }
        probePlugin(plugins, index + 1, cfg, result, cb);
    });
}

void VendorStage::probe(DeviceType type, const IDeviceConfig &cfg, const OnProbe &cb) {
    IDeviceConfig device_cfg = cfg;
    device_cfg.type = type;
    probePlugin(DeviceFactory::plugins(type), 0, device_cfg, ProbeResult(), cb);
}

} // namespace managerkit
