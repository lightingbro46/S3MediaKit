#include <string>
#include <vector>
#include <stdint.h>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <map>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <unistd.h>
#endif

#if defined(__linux__) || defined(__ANDROID__)
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>
#include <fstream>
#elif defined(__APPLE__)
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_var.h>
#endif

#include "NetworkMonitor.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

static bool is_virtual_interface(const std::string& name) {
    static const std::vector<std::string> virtual_prefixes = {
        "lo", "docker", "veth", "br-", "vmnet", "virbr", "zt", "tun", "tap"
    };
    for (const auto& prefix : virtual_prefixes) {
        if (name.compare(0, prefix.size(), prefix) == 0)
            return true;
    }
    return false;
}

static std::vector<NetInterfaceInfo> get_network_interfaces() {
    std::vector<NetInterfaceInfo> interfaces;

#if defined(_WIN32)
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size);
    std::vector<char> buffer(size);
    IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR)
        return interfaces;

    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        NetInterfaceInfo iface;
        iface.name = adapter->Description;
        iface.is_up = (adapter->OperStatus == IfOperStatusUp);
        iface.mac_address = "";
        for (ULONG i = 0; i < adapter->PhysicalAddressLength; ++i) {
            char byte[4];
            snprintf(byte, sizeof(byte), "%02X", adapter->PhysicalAddress[i]);
            iface.mac_address += byte;
            if (i + 1 < adapter->PhysicalAddressLength)
                iface.mac_address += ":";
        }

        for (auto* ua = adapter->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
            char buf[INET6_ADDRSTRLEN] = {};
            getnameinfo(ua->Address.lpSockaddr,
                        (ua->Address.lpSockaddr->sa_family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6),
                        buf, sizeof(buf), nullptr, 0, NI_NUMERICHOST);
            if (ua->Address.lpSockaddr->sa_family == AF_INET)
                iface.ipv4 = buf;
            else
                iface.ipv6 = buf;
        }

        // Tốc độ không có sẵn dễ dàng, cần GetIfTable (bỏ qua ở đây)
        interfaces.push_back(iface);
    }

#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return interfaces;

    std::map<std::string, NetInterfaceInfo> iface_map;

    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        std::string name(ifa->ifa_name);
        auto& iface = iface_map[name];
        iface.name = name;

        if (ifa->ifa_addr) {
            int family = ifa->ifa_addr->sa_family;
            char host[NI_MAXHOST] = {};
            if (family == AF_INET || family == AF_INET6) {
                getnameinfo(ifa->ifa_addr,
                            (family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6),
                            host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
                if (family == AF_INET) iface.ipv4 = host;
                else iface.ipv6 = host;
            }
#if defined(__linux__) || defined(__ANDROID__)
            else if (family == AF_PACKET) {
                auto* s = (struct sockaddr_ll*)ifa->ifa_addr;
                char mac[18];
                snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         s->sll_addr[0], s->sll_addr[1], s->sll_addr[2],
                         s->sll_addr[3], s->sll_addr[4], s->sll_addr[5]);
                iface.mac_address = mac;
            }
#elif defined(__APPLE__)
            else if (family == AF_LINK) {
                auto* sdl = (struct sockaddr_dl*)ifa->ifa_addr;
                unsigned char* mac = (unsigned char*)LLADDR(sdl);
                char buf[18];
                snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                iface.mac_address = buf;
            }
#endif
        }

        iface.is_up = ifa->ifa_flags & IFF_RUNNING;

#if defined(__linux__) || defined(__ANDROID__)
        std::ifstream speed_file("/sys/class/net/" + name + "/speed");
        if (speed_file)
            speed_file >> iface.speed_mbps;
#endif
    }

    freeifaddrs(ifaddr);
    for (const auto& kv : iface_map)
        if (!is_virtual_interface(kv.first)) {
            interfaces.push_back(kv.second);
        }
#endif

    return interfaces;
}

static std::pair<uint64_t, uint64_t> get_rx_tx_bytes(const std::string& interface_name) {
#if defined(__linux__) || defined(__ANDROID__)
    std::ifstream file("/proc/net/dev");
    std::string line;

    while (std::getline(file, line)) {
        auto pos = line.find(":");
        if (pos == std::string::npos) continue;

        std::string iface = line.substr(0, pos);
        iface.erase(std::remove_if(iface.begin(), iface.end(), ::isspace), iface.end());
        if (iface != interface_name) continue;

        std::istringstream iss(line.substr(pos + 1));
        uint64_t rx = 0, tx = 0;
        iss >> rx;
        for (int i = 0; i < 7; ++i) iss >> tx;
        iss >> tx;

        return {rx, tx};
    }
    return {0, 0};

#elif defined(__APPLE__)
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return {0, 0};

    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || interface_name != ifa->ifa_name) continue;
        if (!ifa->ifa_data) continue;

        auto* data = (struct if_data*)ifa->ifa_data;
        uint64_t rx = data->ifi_ibytes;
        uint64_t tx = data->ifi_obytes;
        freeifaddrs(ifaddr);
        return {rx, tx};
    }

    freeifaddrs(ifaddr);
    return {0, 0};

#elif defined(_WIN32)
    ULONG size = 0;
    GetIfTable(nullptr, &size, false);
    std::vector<char> buffer(size);
    MIB_IFTABLE* table = reinterpret_cast<MIB_IFTABLE*>(buffer.data());
    if (GetIfTable(table, &size, false) != NO_ERROR) return {0, 0};

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_IFROW& row = table->table[i];
        std::string name(reinterpret_cast<const char*>(row.bDescr));
        if (name == interface_name) {
            return {row.dwInOctets, row.dwOutOctets};
        }
    }
    return {0, 0};
#else
    return {0, 0};
#endif
}

void NetworkCollector::collect() {
    std::pair<uint64_t, uint64_t> pair1 = get_rx_tx_bytes(_info.name);
    uint64_t rx1 = pair1.first;
    uint64_t tx1 = pair1.second;
    _ticker.resetTime();
    _poller->doDelayTask(1000, [=]() {
        std::pair<uint64_t, uint64_t> pair2 = get_rx_tx_bytes(_info.name);
        uint64_t rx2 = pair2.first;
        uint64_t tx2 = pair2.second;
        double seconds = (double)_ticker.elapsedTime() / 1000;
        double rx_mbps = ((double)(rx2 - rx1) * 8.0) / 1000000.0 / seconds;
        double tx_mbps = ((double)(tx2 - tx1) * 8.0) / 1000000.0 / seconds;
        _info.rx_mbps = rx_mbps;
        _info.tx_mbps = tx_mbps;

        onCollect(_info);
        return 0;
    });
}

void NetworkMonitor::start() {
    auto netifs = get_network_interfaces();
    for (const auto &netif : netifs) {
        auto collector = std::make_shared<NetworkCollector>(netif.name, _poller);
        collector->setOnCollect([&](NetSpeed &info) {
            lock_guard<mutex> lck(_mtx);
            _map_result[info.name].rx_mbps = info.rx_mbps;
            _map_result[info.name].tx_mbps = info.tx_mbps;
        });

        _map_collector[netif.name] = collector;
        _map_result[netif.name] = netif;
    }
}

vector<NetInterfaceInfo> NetworkMonitor::getCurrentUsage() {
    lock_guard<mutex> lck(_mtx);
    vector<NetInterfaceInfo> ret;
    for (const auto &it : _map_result) {
        ret.push_back(it.second);
    }
    return ret;
}

} // namespace managerkit
