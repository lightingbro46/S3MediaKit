#ifndef SERVER_NETWORKMONITOR_H
#define SERVER_NETWORKMONITOR_H

#include <string>
#include <memory>
#include <unordered_map>
#include "ResourceMonitor.h"
#include "Util/TimeTicker.h"

namespace managerkit {

struct NetSpeed {
    std::string name;
    double rx_mbps = 0.0;
    double tx_mbps = 0.0;
};

struct NetInterfaceInfo {
    std::string name;
    std::string ipv4;
    std::string ipv6;
    std::string mac_address;
    bool is_up = false;
    double speed_mbps = 0;
    double rx_mbps = 0.0;
    double tx_mbps = 0.0;
};

class NetworkCollector : public MetricCollector<NetSpeed> {
public:
    using Ptr = std::shared_ptr<NetworkCollector>;
    NetworkCollector(const std::string &name, toolkit::EventPoller::Ptr poller) 
        : MetricCollector(poller) {
        _info.name = name;
    }

private:
    void collect() override;

private:
    NetSpeed _info;
    toolkit::Ticker _ticker;
};

class NetworkMonitor : public ResourceMonitor {
public:
    using Ptr = std::shared_ptr<NetworkMonitor>;
    NetworkMonitor(toolkit::EventPoller::Ptr poller) : ResourceMonitor(poller) {}
    
    void start() override;

    std::vector<NetInterfaceInfo> getCurrentUsage();

private:
    std::unordered_map<std::string, NetworkCollector::Ptr> _map_collector;
    std::unordered_map<std::string, NetInterfaceInfo> _map_result;
};

} // namespace managerkit

#endif // SERVER_NETWORKMONITOR_H