#include "ClusterManager.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceStatus.h"
#include "Storage/VmsResourceType.h"
#include "Storage/VmsKvPair.h"
#include "Extension/SyncManager.h"

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

static void savePeerList(const std::unordered_map<std::string, MediaServerInfo> &peer_map) {
    Json::Value peer_list(Json::arrayValue);
    for (const auto &pr : peer_map) {
        peer_list.append(pr.second.toJson());
    }
    mINI::Instance()[Peer::kPeerList] = StrJsonUtils::writeJsonString(peer_list);
    // Save to file
    mINI::Instance().dumpFile(g_ini_file);
}

/**
 * ClusterManager manages the cluster of media servers, including:
 * 1) Keeping track of media server info and health status, and saving to config file
 * 2) Providing API to get media server info and healthy peer URLs for sync and broadcast
 * 3) Performing health check for media servers and updating peer URLs accordingly
 * 4) Syncing peer list to other peers when peer is added/removed or media server info is updated
 * 5) Removing peer from SyncManager when peer is removed
 * Note: ClusterManager does not handle auto-discovery of media servers, and relies on external API to add/remove media servers and update their info. 
 * Health check is performed based on the info provided by external API, and ClusterManager does not automatically resolve 
 * or update media server URLs based on IP/domain changes or port mapping changes.
 */
INSTANCE_IMP(ClusterManager)

ClusterManager::ClusterManager() {
    _timer = std::make_shared<Timer>(
        60.0f,
        []() {
            // Use singleton access in timer callback to avoid capturing a raw
            // pointer across thread boundaries.
            ClusterManager::Instance().onManager();
            return true;
        },
        nullptr);
}

ClusterManager::~ClusterManager() {
    std::lock_guard<std::mutex> lck(_mtx);
    _timer.reset();
    _map_server_info.clear();
    _map_peer_url.clear();
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

std::string ClusterManager::getPeerUrl(const std::string &id) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = _map_peer_url.find(id);
    return it != _map_peer_url.end() ? it->second : "";
}

std::unordered_map<std::string, std::string> ClusterManager::getPeerUrls() {
    std::lock_guard<std::mutex> lck(_mtx);
    return _map_peer_url;
}

void ClusterManager::addMediaServer(const std::string &id, const MediaServerInfo &info, bool skip_save) {
    bool new_peer = false;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        auto it = _map_server_info.find(id);
        if (it == _map_server_info.end()) {
            _map_server_info[id] = info;
            new_peer = true;
        }  else {
            if (it->second == info) {
                TraceL << "Media server info is the same as existing one, skip update";
                return;
            }
            it->second = info;
        }
        DebugL << "Added media server: " << id << ", name: " << info.name;
        if (!skip_save) {
            savePeerList(_map_server_info);
        }
    }

    if (new_peer) {
        auto origin_urls = buildOrginUrls(info);
        auto weak_self = weak_from_this();
        WorkThreadPool::Instance().getPoller()->async([weak_self, id, origin_urls]() {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            self->healthCheck(id, origin_urls);
        });
    }
}
    
void ClusterManager::removeMediaServer(const std::string &id) {
    {
        std::lock_guard<std::mutex> lck(_mtx);
        _map_server_info.erase(id);
        _map_peer_url.erase(id);
        savePeerList(_map_server_info);
        DebugL << "Removed media server: " << id;
    }
    SyncManager::Instance().removePeer(id);
}

void ClusterManager::healthCheck(const std::string &id, const std::string &origin_urls) {
    GET_CONFIG(string, kMediaServerId, General::kMediaServerId);
    if (id == kMediaServerId) {
        TraceL << "Skip health check for self node " << id;
        return;
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
                }
            }
            SyncManager::Instance().addPeer(id, url);
        } else {
            WarnL << "Health check index out of range for " << origin_urls << ": " << idx;
        }
    };

    NOTICE_EMIT(BroadcastHealthCheckMediaServiceArgs, Broadcast::kBroadcastHealthCheckMediaService, origin_urls, invoker);
}

void ClusterManager::loadSavedMediaServerInfo() {
    GET_CONFIG(string, peer_list_json, Peer::kPeerList);
    if (peer_list_json.empty() || peer_list_json == "[]") return;

    Json::Value list;
    if (!StrJsonUtils::readJsonString(peer_list_json, list) || !list.isArray()) {
        WarnL << "ClusterManager: failed to parse persisted peer list";
        return;
    }

    auto weak_self = weak_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self, list]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        for (const auto &item : list) {
            if (!item.isObject()) {
                WarnL << "ClusterManager: invalid peer item in persisted peer list, skip";
                continue;
            }
            auto info = MediaServerInfo::fromJson(item);
            std::string id = info.id;
            DebugL << "ClusterManager: loading persisted peer " << id << " with url " << buildOrginUrls(info);
            self->addMediaServer(id, info, true);
        }
    });
}

void ClusterManager::onManager() {
    std::unordered_map<std::string, std::string> peer_url_copy;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        for (const auto &pr : _map_server_info) {
            if (pr.first == mINI::Instance()[General::kMediaServerId]) {
                // Skip self node
                continue;
            }
            if (_map_peer_url.find(pr.first) != _map_peer_url.end()) {
                // Skip healthy peer
                continue;
            }
            peer_url_copy.emplace(pr.first, buildOrginUrls(pr.second));
        }
    }
    for (const auto &item : peer_url_copy) {
        string id = item.first;
        string origin_urls = item.second;
        auto weak_self = weak_from_this();
        WorkThreadPool::Instance().getPoller()->async([weak_self, id, origin_urls]() {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            self->healthCheck(id, origin_urls);
        });
    }
}

} // namespace managerkit               
                