#include "ClusterManager.h"
#include "Common/macros.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

INSTANCE_IMP(ClusterManager)

ClusterManager::ClusterManager() {}

ClusterManager::~ClusterManager() {
}

void ClusterManager::setServerLocationId(int id) {
    _server_location_id = id;
}

ServerInfo ClusterManager::getMediaServer(const std::string &id) {
    auto it = _map_server_info.find(id);
    return it != _map_server_info.end() ? it->second : ServerInfo();
};

std::vector<std::string> ClusterManager::getListUrl() {
    std::vector<string> list_url;
    for (auto it = _map_server_info.begin(); it != _map_server_info.end(); ++it) {
        const std::string &mac = it->first;
        const ServerInfo &info = it->second;

        std::string build_url;
        const std::string url_domain = info.domain;
        const std::string https_port = info.https_port == 0 ? "" : (":" + std::to_string(info.https_port));
        const std::string http_port  = info.http_port  == 0 ? "" : (":" + std::to_string(info.http_port));

        const std::string url_prefix = (info.client_use_ssl)?"https://": "http://";
        const std::string url_posfix = (info.client_use_ssl)?https_port: http_port;

        if (info.domain.empty()) {
            build_url = url_prefix + info.ip + http_port;
        } else {
            if (url_domain.rfind("https://", 0) == 0) {
                build_url = url_domain + https_port;
            } else if (url_domain.rfind("http://", 0) == 0) {
                build_url = url_domain + http_port;
            } else {
                build_url = url_prefix + url_domain + url_posfix;
            }
        }
        list_url.push_back(build_url);
    }
    return list_url;
};




void ClusterManager::addMediaServer(const Json::Value &data) {
    
    string mac_address = data["id"].asString();
    string ip = data["ip"].asString();
    string domain = data["domain"].asString();

    if (domain.find("localhost") != std::string::npos) {
        WarnL<<"This domain is locahost. It will be skipped!";
        return;
    }
    unsigned int temp = data["rtspPort"].asUInt();
    if (temp > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("rtsp Port out of range");
    }

    uint16_t rtsp_port, rtmp_port, http_port, https_port;

    rtsp_port = data["rtspPort"].empty() ? 0 : static_cast<uint16_t>(data["rtspPort"].asUInt());
    rtmp_port = data["rtmpPort"].empty() ? 0 : static_cast<uint16_t>(data["rtmpPort"].asUInt());
    http_port = data["httpPort"].empty() ? 0 : static_cast<uint16_t>(data["httpPort"].asUInt());
    https_port = data["httpsPort"].empty() ? 0 : static_cast<uint16_t>(data["httpsPort"].asUInt());

    bool client_user_ssl = data["clientUseSsl"].asBool();

    _map_server_info.emplace(mac_address, ServerInfo(mac_address, ip, domain, rtsp_port, rtmp_port, http_port, https_port, client_user_ssl));
}

void ClusterManager::clearAllMediaServer(){
    WarnL << "Clear all server info in cluster";
    for (auto it = _map_server_info.begin(); it != _map_server_info.end();){
        it = _map_server_info.erase(it);
    }
    WarnL << "size of map after clear: "<<_map_server_info.size();

};




} // namespace managerkit