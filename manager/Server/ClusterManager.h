// #ifndef SERVER_CLUSTERMANAGER_H
// #define SERVER_CLUSTERMANAGER_H

// #include <json/json.h>
// #include <memory>
// #include <mutex>
// #include <unordered_map>
// #include "Util/logger.h"
// #include <unordered_set>

// namespace managerkit {

// struct ServerInfo {

//     std::string id, name, ip, domain, url ;
//     uint16_t rtsp_port, rtmp_port, http_port, https_port;
//     uint16_t auto_rtsp_port, auto_rtmp_port, auto_http_port, auto_https_port;
//     bool is_auto_rtsp_port, is_auto_rtmp_port, is_auto_http_port, is_auto_https_port;
//     bool client_use_ssl, status, hasFailover;

//     ~ServerInfo(){}
//     ServerInfo() = default;
//     ServerInfo(
//         std::string id, std::string name, std::string ip, std::string domain, uint16_t rtsp_port, uint16_t rtmp_port, uint16_t http_port, uint16_t https_port,
//         uint16_t auto_rtsp_port, uint16_t auto_rtmp_port, uint16_t auto_http_port, uint16_t auto_https_port,
//         bool is_auto_rtsp_port, bool is_auto_rtmp_port, bool is_auto_http_port, bool is_auto_https_port,
//         bool client_use_ssl, bool status, bool hasFailover)
//         : id(id)
//         , name(name)
//         , ip(ip)
//         , domain(domain)
//         , rtsp_port(rtsp_port)
//         , rtmp_port(rtmp_port)
//         , http_port(http_port)
//         , https_port(https_port)
//         , auto_rtsp_port(auto_rtsp_port)
//         , auto_rtmp_port(auto_rtmp_port)
//         , auto_http_port(auto_http_port)
//         , auto_https_port(auto_https_port)
//         , is_auto_rtsp_port(is_auto_rtsp_port)
//         , is_auto_rtmp_port(is_auto_rtmp_port)
//         , is_auto_http_port(is_auto_http_port)
//         , is_auto_https_port(is_auto_https_port)
//         , client_use_ssl(client_use_ssl)
//         , status(status)
//         , hasFailover(hasFailover)
//         {}

//     bool operator==(const ServerInfo& other) const{
//         return 
//             this->name              == other.name &&
//             this->ip                == other.ip &&
//             this->domain            == other.domain &&
//             this->http_port         == other.http_port &&
//             this->https_port        == other.https_port &&
//             this->rtmp_port         == other.rtmp_port &&
//             this->rtsp_port         == other.rtsp_port &&
//             this->auto_http_port    == other.auto_http_port &&
//             this->auto_https_port   == other.auto_https_port &&
//             this->auto_rtmp_port    == other.auto_rtmp_port &&
//             this->auto_rtsp_port    == other.auto_rtsp_port &&
//             this->is_auto_http_port == other.is_auto_http_port &&
//             this->is_auto_https_port== other.is_auto_https_port &&
//             this->is_auto_rtmp_port == other.is_auto_rtmp_port &&
//             this->is_auto_rtsp_port == other.is_auto_rtsp_port &&
//             this->client_use_ssl    == other.client_use_ssl &&
//             this->status            == other.status &&
//             this->hasFailover       == other.hasFailover;
//     }

//     bool operator!=(const ServerInfo& other) const {
//         return !(*this == other);
//     }
    
//     void updateConfig(ServerInfo new_info) {
//         WarnL << "update config of media server with id: " << id;
//         name                 = new_info.name;
//         ip                   = new_info.ip;
//         domain               = new_info.domain;
//         rtsp_port            = new_info.rtsp_port;
//         rtmp_port            = new_info.rtmp_port;
//         http_port            = new_info.http_port;
//         https_port           = new_info.https_port;
//         auto_rtsp_port       = new_info.auto_rtsp_port;
//         auto_rtmp_port       = new_info.auto_rtmp_port;
//         auto_http_port       = new_info.auto_http_port;
//         auto_https_port      = new_info.auto_https_port;
//         is_auto_rtsp_port    = new_info.is_auto_rtsp_port;
//         is_auto_rtmp_port    = new_info.is_auto_rtmp_port;
//         is_auto_http_port    = new_info.is_auto_http_port;
//         is_auto_https_port   = new_info.is_auto_https_port;
//         client_use_ssl       = new_info.client_use_ssl;
//         status               = new_info.status;
//         hasFailover          = new_info.hasFailover;
//         //url = new_info.url; // url ko update, chỉ update sau khi healh check xem có thông luồng
//     }
// };

// class ClusterManager : public std::enable_shared_from_this<ClusterManager> {

// public:
//     enum class UrlType{
//         Domain,
//         IpPort
//     };

// public:
//     using Ptr = std::shared_ptr<ClusterManager>;



//     static ClusterManager &Instance();
//     ~ClusterManager();

//     /**
//      *
//      * Get media server
//      */
//     ServerInfo getMediaServer(const std::string &id);

//      /**
//      *
//      * Get url of media server
//      */
//     std::string getUrl(const std::string &id);

//     std::string getUrlFromServerIpPort(const std::string &id);

//     std::string getUrlFromServerDomain(const std::string &id);

//     bool setUrl(const std::string &id, const std::string& newUrl);

//     /**
//      *
//      * Get list url
//      */
//     std::vector<std::string> getListUrl(const bool filterOnline = true);

//      /**
//      *
//      * Get list media server
//      */
//     std::unordered_map<std::string, managerkit::ServerInfo> getListMediaServer();


//     /**
//      * 
//      * Get list media server ids
//      */
//     std::vector<std::string> getListMediaServerIds(const bool filterOnline = true);

//      /**
//      *
//      * Create server info from json value 
//      */
//     ServerInfo createServerInfoFromJson(const Json::Value &server_info);

//     /**
//      *
//      * Add media server to cluster
//      */
//     void addMediaServer(const Json::Value &server_info);

//     /**
//      * 
//      * Set status 
//      */
//     void setStatus(const std::string& server_id, bool new_status);

//     /**
//      * 
//      * Check status 
//      */
//     bool checkStatus(const std::string &id);

//     /**
//      * clear server by key 
//      */
//     void delServer(const std::string& key);

//     /**
//      *
//      * Set server location id
//      */
//     void setServerLocationId(int id);

//     /**
//      *  
//      * Update servers in db esc from ClusterManager
//      */
//     void saveServerInfoToESC();

//     /**
//      * Check connection to all media server in cluster
//      */
//     void healthCheck();

//     /**
//      * get list possible url 
//      */
//     std::vector<std::string> getListPossibleUrl(std::string server_id);

// private:
//     ClusterManager();
//     std::string _media_server_id;
//     std::string _server_location_id;
//     std::unordered_map<std::string /*id*/, ServerInfo> _map_server_info;
// };

// } // namespace managerkit

// #endif // SERVER_CLUSTERMANAGER_H