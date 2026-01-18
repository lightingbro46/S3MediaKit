// #include "ClusterManager.h"
// #include "Common/macros.h"
// #include "Storage/VmsResource.h"
// #include "Storage/VmsResourceStatus.h"
// #include "Storage/VmsResourceType.h"
// #include "Storage/VmsKvPair.h"
// using namespace std;
// using namespace toolkit;

// namespace managerkit {

// INSTANCE_IMP(ClusterManager)

// ClusterManager::ClusterManager() {}

// ClusterManager::~ClusterManager() {}

// void ClusterManager::setServerLocationId(int id) {
//     _server_location_id = id;
// }

// ServerInfo ClusterManager::getMediaServer(const std::string &id) {
//     auto it = _map_server_info.find(id);
//     return it != _map_server_info.end() ? it->second : ServerInfo();
// };

// static string buildUrl(ServerInfo &info, ClusterManager::UrlType urlType = ClusterManager::UrlType::Domain, bool is_auto_port = false);

// std::string ClusterManager::getUrl(const std::string &id) {
//     if (id.empty()) // TH id rỗng
//         return "";
//     ServerInfo serverInfo = getMediaServer(id);
//     if (serverInfo.id.empty())  // TH không tồn tại server trong cluster
//         return "";
//     return serverInfo.url;
// }

// bool ClusterManager::setUrl(const std::string &id, const std::string &newUrl) {
//     if (id.empty())
//         return false;
//     auto it = _map_server_info.find(id);
//     if (it ==_map_server_info.end()) return false;

//     DebugL<<"Update url: "<< newUrl << " for mediaserver: "<<id;
//     _map_server_info[id].url = newUrl;
//     return true;
// }

// std::string ClusterManager::getUrlFromServerDomain(const std::string &id) {
//     std::string url;
//     if (id.empty())
//         return url;
//     ServerInfo serverInfo = getMediaServer(id);
//     if (serverInfo.id.empty())
//         return "";
//     url = buildUrl(serverInfo, ClusterManager::UrlType::Domain);
//     return url;
// }

// std::string ClusterManager::getUrlFromServerIpPort(const std::string &id) {
//     std::string url;
//     if (id.empty())
//         return url;
//     ServerInfo serverInfo = getMediaServer(id);
//     if (serverInfo.id.empty())
//         return "";
//     url = buildUrl(serverInfo, ClusterManager::UrlType::IpPort);
//     return url;
// }

// std::unordered_map<std::string, managerkit::ServerInfo> ClusterManager::getListMediaServer(){
//     return _map_server_info;
// }

// std::vector<string> ClusterManager::getListMediaServerIds(const bool filterOnline){
//     std::vector<string> list_ids;
//     GET_CONFIG(std::string, mediaServerId, General::kMediaServerId);

//     for (auto it = _map_server_info.begin(); it != _map_server_info.end(); ++it) {
//         if (it->first == mediaServerId) continue;
//         if (filterOnline && it->second.status == false) continue;
//         list_ids.push_back(it->first);        
//     }
//     return list_ids;
// }

// std::vector<std::string> ClusterManager::getListUrl(bool filterOnline) {
//     std::vector<string> list_url;
//     GET_CONFIG(std::string, mediaServerId, General::kMediaServerId);

//     for (auto it = _map_server_info.begin(); it != _map_server_info.end(); ++it) {
//         const std::string &mac = it->first;
//         const bool status = it->second.status;
//         if (mac == mediaServerId) continue;
//         if (filterOnline && status == false ) continue;
//         const std::string url = it->second.url;
//         list_url.push_back(url);
//     }
//     return list_url;
// };

// ServerInfo ClusterManager::createServerInfoFromJson(const Json::Value &data) {
//     string id = data["id"].asString();
//     string name = data["name"].asString();
//     string ip = data["ip"].asString();
//     string domain = data["domain"].asString();

//     // if (domain.find("localhost") != std::string::npos) {
//     //     WarnL << "This domain is localhost. It will be skipped!";
//     //     ServerInfo info;
//     //     return info;
//     // }
 
//     unsigned int temp = data["rtspPort"].asUInt();
//     if (temp > std::numeric_limits<uint16_t>::max()) {
//         throw std::runtime_error("rtsp Port out of range");
//     }

//     uint16_t rtsp_port, rtmp_port, http_port, https_port;
//     uint16_t auto_rtsp_port, auto_rtmp_port, auto_http_port, auto_https_port;
//     bool is_auto_rtsp_port, is_auto_rtmp_port, is_auto_http_port , is_auto_https_port;

//     rtsp_port = data["rtspPort"].empty() ? 0 : static_cast<uint16_t>(data["rtspPort"].asUInt());
//     rtmp_port = data["rtmpPort"].empty() ? 0 : static_cast<uint16_t>(data["rtmpPort"].asUInt());
//     http_port = data["httpPort"].empty() ? 0 : static_cast<uint16_t>(data["httpPort"].asUInt());
//     https_port = data["httpsPort"].empty() ? 0 : static_cast<uint16_t>(data["httpsPort"].asUInt());

//     auto_rtsp_port  = data["autoRtspPort"].empty() ? 0 : static_cast<uint16_t>(data["autoRtspPort"].asUInt()); 
//     auto_rtmp_port  = data["autoRtmpPort"].empty() ? 0 : static_cast<uint16_t>(data["autoRtmpPort"].asUInt());
//     auto_http_port  = data["autoHttpPort"].empty() ? 8080 : static_cast<uint16_t>(data["autoHttpPort"].asUInt());
//     auto_https_port = data["autoHttpsPort"].empty() ? 0 : static_cast<uint16_t>(data["autoHttpsPort"].asUInt());

//     is_auto_rtsp_port  =  data["isAutoRtspPort"].empty() ? false : data["isAutoRtspPort"].asBool();
//     is_auto_rtmp_port  =  data["isAutoRtmpPort"].empty() ? false : data["isAutoRtmpPort"].asBool();
//     is_auto_http_port  =  data["isAutoHttpPort"].empty() ? true : data["isAutoHttpPort"].asBool();
//     is_auto_https_port =  data["isAutoHttpsPort"].empty() ? false : data["isAutoHttpsPort"].asBool();

//     bool client_user_ssl = data["clientUseSsl"].asBool();
//     bool status = data["status"].asBool();
//     bool hasFailover = data["hasFailover"].asBool();
//     ServerInfo info(id, name, ip, domain, rtsp_port, rtmp_port, http_port, https_port, auto_rtsp_port, auto_rtmp_port, auto_http_port, auto_https_port, is_auto_rtsp_port, is_auto_rtmp_port, is_auto_http_port, is_auto_https_port,  client_user_ssl, status, hasFailover);
//     std::string url = buildUrl(info); //mặc định ban đầu là rỗng
//     info.url = url;
//     return info;
// }

// void ClusterManager::addMediaServer(const Json::Value &data) {

//     auto server_info = createServerInfoFromJson(data);
//     string id = server_info.id;

//     auto it = _map_server_info.find(id);
//     if (it == _map_server_info.end()) {
//         // if not exist in map, add new server
//         _map_server_info.emplace(server_info.id, server_info);
//     } else {
//         // if exist, update config
//         if (it->second == server_info) return;
//         it->second.updateConfig(server_info);
//     }
// }

// static string buildUrl(ServerInfo &info, ClusterManager::UrlType url_type, bool is_auto_port) {

//     std::ostringstream ss;

//     const string url_domain = info.domain;
//     string https_port, http_port;

//     if (is_auto_port)
//         https_port = info.auto_https_port == 0 ? "" : (":" + std::to_string(info.auto_https_port));
//     else
//         https_port = info.https_port == 0 ? "" : (":" + std::to_string(info.https_port));

//     if (is_auto_port)
//         http_port = info.auto_http_port == 0 ? "" : (":" + std::to_string(info.auto_http_port));
//     else 
//         http_port = info.http_port == 0 ? "" : (":" + std::to_string(info.http_port));

//     string url_prefix, url_posfix;
//     url_prefix = (info.client_use_ssl) ? "https://" : "http://";
//     url_posfix = (info.client_use_ssl) ? https_port : http_port;

//     string build_url;

//     switch (url_type){
//         case ClusterManager::UrlType::Domain:
//             if (info.domain.empty()) {
//                 ss << url_prefix << info.ip << url_posfix;
//             } else {
//                 if (url_domain.rfind("https://", 0) == 0) {
//                     ss << url_domain << https_port;
//                 } else if (url_domain.rfind("http://", 0) == 0) {
//                     ss << url_domain << http_port;
//                 } else {
//                     ss << url_prefix << url_domain << url_posfix;
//                 }
//             }
//             build_url = ss.str();
//             break;
//         case ClusterManager::UrlType::IpPort:
//             ss << url_prefix << info.ip << url_posfix;
//             build_url = ss.str(); 
//             break;   
//     }
//     return build_url;
// }

// bool ClusterManager::checkStatus(const std::string &id) {
//     auto it = _map_server_info.find(id);
//     if (it == _map_server_info.end()) return false;
//     return _map_server_info[id].status;
// }

// void ClusterManager::setStatus(const std::string& server_id, bool new_status){
//     auto it = _map_server_info.find(server_id);
//     if (it == _map_server_info.end()) return;
//     _map_server_info[server_id].status = new_status;
// };

// void ClusterManager::delServer(const std::string& id){
//     auto it = _map_server_info.find(id);

//     if (it != _map_server_info.end()){
//         _map_server_info.erase(it);
//     }
// }

// std::vector<std::string> ClusterManager::getListPossibleUrl(std::string server_id){
//     ServerInfo info = getMediaServer(server_id);
//     std::vector<std::string> list_url;  
//     if (info.id.empty()) return list_url;

//     list_url.push_back(buildUrl(info, ClusterManager::UrlType::Domain));
//     list_url.push_back(buildUrl(info, ClusterManager::UrlType::IpPort));

//     if (info.is_auto_http_port){
//         list_url.push_back(buildUrl(info, ClusterManager::UrlType::Domain, true));
//         list_url.push_back(buildUrl(info, ClusterManager::UrlType::IpPort, true));
//     }
//     return list_url;
// }

// // Check thông luồng
// void ClusterManager::healthCheck() {
//     std::vector<std::string> server_ids = getListMediaServerIds(false); // lấy tất server id trong cluster cả online/offline
//     for (std::string server_id: server_ids){
//         std::vector<std::string> urls = getListPossibleUrl(server_id);

//         auto invoker_ptr = std::make_shared<Broadcast::HealthCheckInvoker>();
//         *invoker_ptr = [urls, server_id, invoker_ptr](const Json::Value &body, const std::string &err, const size_t &index) mutable {
//             if (err.empty()) {
//                 std::string mediaServerId = body["data"]["mediaServerId"].asString();
//                 if (mediaServerId == server_id) {
//                     DebugL << "Can connect to server: " << server_id << "by url: " << urls[index];
//                     ClusterManager::Instance().setStatus(server_id, 1);
//                     ClusterManager::Instance().setUrl(server_id, urls[index]);
//                     return;
//                 }
//             }
//             const size_t next_index = index + 1;
//             if (next_index == urls.size()) {
//                 DebugL << "Can not connect to server: " << server_id;
//                 ClusterManager::Instance().setStatus(server_id, 0);
//                 std::string url = ClusterManager::Instance().getUrlFromServerDomain(server_id);
//                 ClusterManager::Instance().setUrl(server_id, url); // mặc định khi server off thì build url từ domain
//                 return;
//             }
//             NOTICE_EMIT(BroadcastHealthCheckArgs, Broadcast::kBroadcastHealthCheck, server_id, urls, next_index, *invoker_ptr);
//         };
//         const size_t index = 0;
//         NOTICE_EMIT(BroadcastHealthCheckArgs, Broadcast::kBroadcastHealthCheck, server_id, urls, index, *invoker_ptr);
//     }
// }

// void ClusterManager::saveServerInfoToESC() {
//     GET_CONFIG(std::string, mediaServerId, General::kMediaServerId);
//     auto resource_imp = std::make_shared<VmsResourceImp>();
//     auto kv_imp = std::make_shared<VmsKvPairImp>();
//     auto status_imp = std::make_shared<VmsResourceStatusImp>();
//     auto type_imp = std::make_shared<VmsResourceTypeImp>();

//     ServerInfo server_info = ClusterManager::Instance().getMediaServer(mediaServerId);
//     if (server_info.id.empty()) return;

//     // 1. Get info about type server in db esc
//     std::vector<VmsResourceType> types = type_imp->findAll();
//     VmsResourceType server_type;
//     for (auto type : types) {
//         if (type.name == "Server")
//             server_type = type;
//     }

//     // 2. Add new server resource
//     VmsResource resource;
//     resource.guid = server_info.id;
//     resource.name = server_type.description;
//     resource.xtype_guid = server_type.guid;
//     resource.url = ClusterManager::Instance().getUrl(server_info.id);
//     resource_imp->addWithLog(resource);

//     // 3. Add key and value of server
//     VmsKvPair pair;
//     pair.resource_guid = resource.guid;

//     pair.name = "http_port";
//     pair.value = std::to_string(server_info.http_port);
//     kv_imp->addWithLog(pair);

//     pair.name = "https_port";
//     pair.value = std::to_string(server_info.https_port);
//     kv_imp->addWithLog(pair);

//     pair.name = "domain";
//     pair.value = server_info.domain;
//     kv_imp->addWithLog(pair);

//     pair.name = "name";
//     pair.value = server_info.name;
//     kv_imp->addWithLog(pair);

//     pair.name = "ip";
//     pair.value = server_info.ip;
//     kv_imp->addWithLog(pair);

//     pair.name = "rtmp_port";
//     pair.value = std::to_string(server_info.rtmp_port);
//     kv_imp->addWithLog(pair);

//     pair.name = "rtsp_port";
//     pair.value = std::to_string(server_info.rtsp_port);
//     kv_imp->addWithLog(pair);

//     // // 4. Update status
//     // VmsResourceStatus status;
//     // status.guid     = resource.guid;
//     // status.status   = server_info.status; // 0 -> off, 1 -> on, 2 -> unauthorize
//     // status_imp->addWithLog(status);
    
// }

// } // namespace managerkit               
                