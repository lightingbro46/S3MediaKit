#include "ClusterManager.h"
#include "Common/config.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceStatus.h"
#include "Storage/VmsResourceType.h"
#include "Storage/VmsKvPair.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace Peer {
#define PEER_FIELD "peer."
const std::string kPeerList = PEER_FIELD"peer_list";

static onceToken token([]() {
    mINI::Instance()[kPeerList] = "[]";
});
}

namespace managerkit {

INSTANCE_IMP(ClusterManager)

ClusterManager::~ClusterManager() {
    std::lock_guard<std::mutex> lck(_mtx);
    _map_server_info.clear();
}

std::vector<std::string> ClusterManager::getMediaServerIds(){
    std::lock_guard<std::mutex> lck(_mtx);
    std::vector<std::string> ids;
    for (const auto &pr : _map_server_info) {
        ids.push_back(pr.first);
    }
    return ids;
}

MediaServerInfo ClusterManager::getMediaServer(const std::string &id) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = _map_server_info.find(id);
    return it != _map_server_info.end() ? it->second : MediaServerInfo();
};

// Helper to extract domain from api_url, e.g. http://domain:port/path -> domain
static std::string fromApiUrl(const std::string &api_url) {
    if (api_url.empty()) {
        return "";
    }
    // Extract domain from api_url, e.g. http://domain:port/path -> domain
    std::string domain;
    auto start_pos = api_url.find("://");
    if (start_pos != std::string::npos) {
        start_pos += 3; // Skip "://"
    } else {
        start_pos = 0;
    }
    auto end_pos = api_url.find(':', start_pos);
    if (end_pos == std::string::npos) {
        end_pos = api_url.find('/', start_pos);
    }
    if (end_pos != std::string::npos) {
        domain = api_url.substr(start_pos, end_pos - start_pos);
    } else {
        domain = api_url.substr(start_pos);
    }
    return domain;
}

// Helper to build base URL for health check and sync based on media server info
static std::string buildOrginUrls(const MediaServerInfo &info) {
    std::string base_url;
    // Start with protocol
    base_url = info.clientUseSsl ? "https://" : "http://";
    // Domain or IP
    GET_CONFIG(string, api_url, "hook.api_url");
    if (info.useWebDomain && !api_url.empty()) {
        auto only_api_domain = fromApiUrl(api_url);
        base_url += only_api_domain;
    } else if (info.useDomain) {
        base_url += info.domain;
    } else {
        base_url += info.ip;
    }
    // Port
    if (info.clientUseSsl) {
        base_url += ":" + (info.isAutoHttpsPort ? to_string(info.httpsPort) : to_string(info.natHttpsPort));
    } else {
        base_url += ":" + (info.isAutoHttpPort ? to_string(info.httpPort) : to_string(info.natHttpPort));
    }
    // Custom path
    if (info.useCustomPath) {
        base_url += info.customPath;
    }
    return base_url;
}

void ClusterManager::addMediaServer(const std::string &id, const MediaServerInfo &info) {
    bool new_peer = false;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        auto it = _map_server_info.find(id);
        if (it == _map_server_info.end()) {
            _map_server_info[id] = info;
            new_peer = true;
        }  else {
            if (it->second == info) {
                DebugL << "Media server info is the same as existing one, skip update";
                return;
            }
            it->second = info;
        }
        TraceL << "Added media server: " << id << ", name: " << info.name;
    }

    if (new_peer) {
        healthCheck(id);
    }
}
    
void ClusterManager::removeMediaServer(const std::string &id) {
    {
        std::lock_guard<std::mutex> lck(_mtx);
        _map_server_info.erase(id);
        TraceL << "Removed media server: " << id;
    }
}

static void savePeerList(const std::unordered_map<std::string, std::string> &peer_url_map) {
    Json::Value peer_list(Json::arrayValue);
    for (const auto &pr : peer_url_map) {
        Json::Value peer_info;
        peer_info["id"] = pr.first;
        peer_info["url"] = pr.second;
        peer_list.append(peer_info);
    }
    mINI::Instance()[Peer::kPeerList] = peer_list.toStyledString();
    // Save to file
    mINI::Instance().dumpFile();
}

void ClusterManager::healthCheck(const std::string &id) {
    std::string origin_urls;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        auto it = _map_server_info.find(id);
        if (it == _map_server_info.end()) {
            WarnL << "Media server not found for health check: " << id;
            return;
        }
        origin_urls = buildOrginUrls(it->second);
    }

    weak_ptr<ClusterManager> weak_self = shared_from_this();
    Broadcast::HealthInvoker invoker = [weak_self, origin_urls, id](const std::string& err, const int& idx) {
        auto self = weak_self.lock();
        if (!self) return;
        if (!err.empty()) {
            WarnL << "Health check failed for " << origin_urls << ": " << err;
            return;
        }

        std::vector<std::string> urls = split(origin_urls, ",");
        if (!urls.empty() && static_cast<size_t>(idx) < urls.size()) {
            auto url = urls[idx];
            DebugL << "Health check succeeded for " << url;
            {
                std::lock_guard<std::mutex> lck(self->_mtx);
                if (self->_map_server_info.find(id) != self->_map_server_info.end()) {
                    self->_map_peer_url[id] = urls[idx];
                    savePeerList(self->_map_peer_url);
                }
            }
            SyncManager::Instance().addPeer(id, url);
        } else {
            WarnL << "Health check index out of range for " << origin_urls << ": " << idx;
        }
    };

    NOTICE_EMIT(BroadcastHealthCheckServiceArgs, Broadcast::kBroadcastHealthCheckService, origin_urls, invoker);
}

} // namespace managerkit               
                