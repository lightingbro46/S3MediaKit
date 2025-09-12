#ifndef SERVER_CLUSTERMANAGER_H
#define SERVER_CLUSTERMANAGER_H

#include <json/json.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "Util/logger.h"

namespace managerkit {

struct ServerInfo {

    std::string mac_address, ip, domain;
    uint16_t rtsp_port, rtmp_port, http_port, https_port;
    bool client_use_ssl;
    ~ServerInfo(){
        WarnL<<"Destroy server info: "<<domain;
    }
    ServerInfo() = default;
    ServerInfo(
        std::string mac_address, std::string ip, std::string domain, uint16_t rtsp_port, uint16_t rtmp_port, uint16_t http_port, uint16_t https_port,
        bool client_use_ssl)
        : mac_address(mac_address)
        , ip(ip)
        , domain(domain)
        , rtsp_port(rtsp_port)
        , rtmp_port(rtmp_port)
        , http_port(http_port)
        , https_port(https_port)
        , client_use_ssl(client_use_ssl) {}
};

class ClusterManager : public std::enable_shared_from_this<ClusterManager> {

public:
    using Ptr = std::shared_ptr<ClusterManager>;

    static ClusterManager &Instance();
    ~ClusterManager();

    /**
     *
     * get media server
     */
    ServerInfo getMediaServer(const std::string &id);

    /**
     *
     * get list media server
     */
    std::vector<std::string> getListUrl();

    /**
     *
     * add media server to cluster
     */
    void addMediaServer(const Json::Value &server_info);

    /**
     * 
     * clear media server  
     */
    void clearAllMediaServer();


    /**
     *
     * set server location id
     */
    void setServerLocationId(int id);

private:
    ClusterManager();
    std::string _media_server_id;
    std::string _server_location_id;

    std::unordered_map<std::string /*id*/, ServerInfo> _map_server_info;
};

} // namespace managerkit

#endif // SERVER_CLUSTERMANAGER_H
