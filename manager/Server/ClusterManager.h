#ifndef SERVER_CLUSTERMANAGER_H
#define SERVER_CLUSTERMANAGER_H

#include <memory>
#include <mutex>
#include <unordered_map>
#include "Util/logger.h"
#include "Poller/Timer.h"

namespace Peer {
// List of peers in the cluster, stored as JSON string in config, format: [{"id": "peer1", "url": "http://ip:port"}, ...]
extern const std::string kPeerList;
} // namespace Peer

namespace managerkit {

struct MediaServerInfo {
    std::string id;
    std::string name;
    std::string ip;
    std::string domain;
    uint16_t rtspPort = 0;
    uint16_t rtmpPort = 0;
    uint16_t httpPort = 0;
    uint16_t httpsPort = 0;
    uint16_t natRtspPort = 0;
    uint16_t natRtmpPort = 0;
    uint16_t natHttpPort = 0;
    uint16_t natHttpsPort = 0;
    bool isAutoRtspPort = true;
    bool isAutoRtmpPort = true;
    bool isAutoHttpPort = true;
    bool isAutoHttpsPort = true;
    bool clientUseSsl = false;
    int status = 0; // 0 -> off, 1 -> on, 2 -> unauthorize
    bool useWebDomain; // use web domain to access media server, if false, use domain or ip to access media server
    bool useDomain; // whether to use domain to access media server, if false, use ip
    bool useCustomPath;
    std::string customPath; // if useCustomPath is true, use this custom path to access media server, e.g. "http://domain/customPath"
    bool hasFailover = false;
    int serverGroupId = 0;

    bool operator==(const MediaServerInfo& other) const {
        return id == other.id &&
               name == other.name &&
               ip == other.ip &&
               domain == other.domain &&
               rtspPort == other.rtspPort &&
               rtmpPort == other.rtmpPort &&
               httpPort == other.httpPort &&
               httpsPort == other.httpsPort &&
               natRtspPort == other.natRtspPort &&
               natRtmpPort == other.natRtmpPort &&
               natHttpPort == other.natHttpPort &&
               natHttpsPort == other.natHttpsPort &&
               isAutoRtspPort == other.isAutoRtspPort &&
               isAutoRtmpPort == other.isAutoRtmpPort &&
               isAutoHttpPort == other.isAutoHttpPort &&
               isAutoHttpsPort == other.isAutoHttpsPort &&
               clientUseSsl == other.clientUseSsl &&
               status == other.status &&
               useWebDomain == other.useWebDomain &&
               useDomain == other.useDomain &&
               useCustomPath == other.useCustomPath &&
               customPath == other.customPath &&
               hasFailover == other.hasFailover;
    }
};

class ClusterManager : public std::enable_shared_from_this<ClusterManager> {
public:
    using Ptr = std::shared_ptr<ClusterManager>;

    static ClusterManager &Instance();
    ~ClusterManager();

    std::vector<std::string> getMediaServerIds();

    MediaServerInfo getMediaServer(const std::string &id);

    void addMediaServer(const std::string &id, const MediaServerInfo &info);
    
    void removeMediaServer(const std::string &id);

private:
    ClusterManager() = default;

    void healthCheck(const std::string &id);

    void onManager();

private:
    std::mutex _mtx;
    toolkit::Timer::Ptr _timer;
    std::unordered_map<std::string /*id*/, MediaServerInfo> _map_server_info;
    std::unordered_map<std::string /*id*/, std::string /*base_url*/> _map_peer_url;
};

} // namespace managerkit

#endif // SERVER_CLUSTERMANAGER_H