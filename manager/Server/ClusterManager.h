#ifndef SERVER_CLUSTERMANAGER_H
#define SERVER_CLUSTERMANAGER_H

#include <memory>
#include <mutex>
#include <unordered_map>
#include "Util/logger.h"
#include "Poller/Timer.h"
#include "json/json.h"

namespace Peer {
// List of peers in the cluster, stored as JSON string in config, format: [{"id": "peer1", "name": "name", "ip": "ip",...}, ...]
extern const std::string kPeerList;
} // namespace Peer

extern std::string g_ini_file;

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
               useWebDomain == other.useWebDomain &&
               useDomain == other.useDomain &&
               useCustomPath == other.useCustomPath &&
               customPath == other.customPath &&
               hasFailover == other.hasFailover;
    }

    Json::Value toJson() const {
        Json::Value v;
        v["id"] = id;
        v["name"] = name;
        v["ip"] = ip;
        v["domain"] = domain;
        v["rtsp_port"] = rtspPort;
        v["rtmp_port"] = rtmpPort;
        v["http_port"] = httpPort;
        v["https_port"] = httpsPort;
        v["nat_rtsp_port"] = natRtspPort;
        v["nat_rtmp_port"] = natRtmpPort;
        v["nat_http_port"] = natHttpPort;
        v["nat_https_port"] = natHttpsPort;
        v["isAutoRtspPort"] = isAutoRtspPort;
        v["isAutoRtmpPort"] = isAutoRtmpPort;
        v["isAutoHttpPort"] = isAutoHttpPort;
        v["isAutoHttpsPort"] = isAutoHttpsPort;
        v["clientUseSsl"] = clientUseSsl;
        v["useWebDomain"] = useWebDomain;
        v["useDomain"] = useDomain;
        v["useCustomPath"] = useCustomPath;
        v["customPath"] = customPath;
        v["hasFailover"] = hasFailover;
        return v;
    }

    static MediaServerInfo fromJson(const Json::Value &v) {
        MediaServerInfo info;
        info.id = v["id"].asString();
        info.name = v["name"].asString();
        info.ip = v["ip"].asString();
        info.domain = v["domain"].asString();
        info.rtspPort = v["rtsp_port"].asUInt();
        info.rtmpPort = v["rtmp_port"].asUInt();
        info.httpPort = v["http_port"].asUInt();
        info.httpsPort = v["https_port"].asUInt();
        info.natRtspPort = v["nat_rtsp_port"].asUInt();
        info.natRtmpPort = v["nat_rtmp_port"].asUInt();
        info.natHttpPort = v["nat_http_port"].asUInt();
        info.natHttpsPort = v["nat_https_port"].asUInt();
        info.isAutoRtspPort = v["isAutoRtspPort"].asBool();
        info.isAutoRtmpPort = v["isAutoRtmpPort"].asBool();
        info.isAutoHttpPort = v["isAutoHttpPort"].asBool();
        info.isAutoHttpsPort = v["isAutoHttpsPort"].asBool();
        info.clientUseSsl = v["clientUseSsl"].asBool();
        info.useWebDomain = v["useWebDomain"].asBool();
        info.useDomain = v["useDomain"].asBool();
        info.useCustomPath = v["useCustomPath"].asBool();
        info.customPath = v["customPath"].asString();
        info.hasFailover = v["hasFailover"].asBool();
        return info;
    }
};

class ClusterManager : public std::enable_shared_from_this<ClusterManager> {
public:
    using Ptr = std::shared_ptr<ClusterManager>;

    static ClusterManager &Instance();
    ~ClusterManager();

    std::vector<std::string> getMediaServerIds();

    MediaServerInfo getMediaServer(const std::string &id);

    /**
     * Returns the resolved base URL (e.g. "http://ip:port") for the given peer id,
     * or an empty string if the peer is not known / health-check has not succeeded yet.
     */
    std::string getPeerUrl(const std::string &id);

    /**
     * Returns a snapshot of known peer id → base_url mappings (health-checked peers only).
     */
    std::unordered_map<std::string, std::string> getPeerUrls();

    void addMediaServer(const std::string &id, const MediaServerInfo &info, bool skip_save = false);
    
    void removeMediaServer(const std::string &id);

    void loadSavedMediaServerInfo();

private:
    ClusterManager();

    void healthCheck(const std::string &id, const std::string &origin_urls);

    void onManager();

private:
    std::mutex _mtx;
    toolkit::Timer::Ptr _timer;
    // Map of media server id to its info, used for config management and health check. 
    // Updated when peer is added/removed or media server info is updated, and saved to config file.
    std::unordered_map<std::string /*id*/, MediaServerInfo> _map_server_info;
    // Map of peer id to its base URL (e.g. "http://ip:port"), only for healthy peers that passed health check, used for sync and broadcast. 
    // Updated by healthCheck() and cleared when peer is removed.
    std::unordered_map<std::string /*id*/, std::string /*base_url*/> _map_peer_url;
};

} // namespace managerkit

#endif // SERVER_CLUSTERMANAGER_H