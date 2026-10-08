#ifndef CONTROL_VENDORSTAGE_H
#define CONTROL_VENDORSTAGE_H

#include <functional>
#include <string>
#include "ext-plugin/IDevice.h"

namespace managerkit {

// The vendor plugin step a device controller runs before the control protocols: which brand the
// device is (configured, or detected when the manufacturer is blank), and what its plugin needs.
// Plain data, no thread or timer: the owner calls it on its own poller, as ProtocolLock.
class VendorStage {
public:
    // What the plugin APIs answered to the configured account.
    struct ProbeResult {
        DeviceBrand brand = DeviceBrand::UNKNOWN; // brand whose API accepted the login; UNKNOWN = none
        bool rejected = false;                    // a plugin API answered 401/403
        bool answered = false;                    // some plugin API answered (not refused, no timeout)
    };
    typedef std::function<void(const ProbeResult &result)> OnProbe;

    // A device of `type` configured with `manufacturer`: its brand and plugin, and whether to detect
    // them (blank manufacturer, and `type` has plugins to probe). Forgets what an earlier one learned.
    void configure(DeviceType type, const std::string &manufacturer);

    bool detecting() const { return _detecting; }
    DeviceBrand brand() const { return _brand; }
    bool hasPlugin() const { return _has_plugin; }
    // Valid when hasPlugin().
    const VendorPluginInfo &plugin() const { return _plugin; }
    // Manufacturer detected (written back to the device config); "" before.
    const std::string &name() const { return _name; }

    // A detection cycle starts: the plugin APIs are probed.
    void startCycle() { _cycle_answered = false; }
    // The plugin APIs answered: true when one accepted the login (detected, see brand()/name()).
    // A rejected login is not taken as the brand: some web servers answer 401 to any path.
    bool onVendorProbe(const ProbeResult &result);
    // A control protocol connected while detecting; `manufacturer` is its device information. Stops
    // detecting either way: true when there is a name to write back (brand()/hasPlugin()/name()).
    bool identify(const std::string &manufacturer);
    // A detection cycle ended without a controller; `unreachable`: no control protocol answered.
    void onCycleFailed(bool unreachable);
    void resetBackoff() { _unreachable_cycles = 0; }
    // Seconds between maintenance cycles: 60, doubled per detection cycle nothing answered, up to
    // `max_interval_sec` (60 when it is 0 or less).
    int cycleIntervalSec(int max_interval_sec) const;

    // Logs in to the API of every plugin of `type` with the account of `cfg`, in turn, until one
    // accepts. `cb` runs once, on any thread.
    static void probe(DeviceType type, const IDeviceConfig &cfg, const OnProbe &cb);

private:
    void setBrand(DeviceBrand brand, const std::string &name);

private:
    DeviceType _type = DeviceType::Speaker;
    DeviceBrand _brand = DeviceBrand::UNKNOWN;
    bool _has_plugin = false;
    VendorPluginInfo _plugin;
    std::string _name;
    bool _detecting = false;
    bool _cycle_answered = false;   // a plugin API answered in the current cycle
    int _unreachable_cycles = 0;    // detection cycles in a row nothing answered
};

} // namespace managerkit

#endif // CONTROL_VENDORSTAGE_H
