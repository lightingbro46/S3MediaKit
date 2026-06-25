#ifndef SERVER_NETWORKMONITOR_H
#define SERVER_NETWORKMONITOR_H

#include <string>
#include <memory>
#include <unordered_map>
#include "json/json.h"
#include "ResourceMonitor.h"
#include "Util/TimeTicker.h"

namespace managerkit {

struct NetSpeed {
    std::string name;
    float rx_mbps = 0.0f;
    float tx_mbps = 0.0f;
};

struct NetInterfaceInfo {
    std::string name;
    std::string ipv4;
    std::string ipv6;
    std::string mac_address;
    bool is_up = false;
    float speed_mbps = 0.0f;
    float rx_mbps = 0.0f;
    float tx_mbps = 0.0f;

    Json::Value toJson() const {
        Json::Value nv;
        nv["name"]      = name;
        nv["ipv4"]      = ipv4;
        nv["ipv6"]      = ipv6;
        nv["mac"]       = mac_address;
        nv["rx_mbps"]   = sanitize_for_json(rx_mbps);
        nv["tx_mbps"]   = sanitize_for_json(tx_mbps);
        nv["speed_mbps"]= speed_mbps;
        return nv;
    }
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
    NetworkMonitor(toolkit::EventPoller::Ptr poller) : ResourceMonitor(ResourceType::NETWORK, poller) {
        start();
    }

    std::vector<NetInterfaceInfo> getCurrentUsage();

private:
    void start() override;

private:
    std::unordered_map<std::string, NetworkCollector::Ptr> _map_collector;
    std::unordered_map<std::string, NetInterfaceInfo> _map_result;
};

} // namespace managerkit

#endif // SERVER_NETWORKMONITOR_H