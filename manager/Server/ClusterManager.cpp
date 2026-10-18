#include "ClusterManager.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceStatus.h"
#include "Storage/VmsResourceType.h"
#include "Storage/VmsKvPair.h"
#include <tuple>
#ifdef ENABLE_SYNC_CHANGES
#include "Extension/SyncManager.h"
#endif

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace Peer {
#define PEER_FIELD "peer."
const std::string kPeerList = PEER_FIELD"peer_list";
const std::string kDecommissionedSince = PEER_FIELD"decommissioned_since";

static onceToken token([]() {
    mINI::Instance()[kPeerList] = "[]";
    mINI::Instance()[kDecommissionedSince] = 0;
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
    _map_peer_revision.clear();
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
    bool changed_peer = false;
    uint64_t revision = 0;
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
            changed_peer = true;
        }
        revision = ++_next_peer_revision;
        _map_peer_revision[id] = revision;
        _map_peer_url.erase(id);
        DebugL << "Added media server: " << id << ", name: " << info.name;
        if (!skip_save) {
            savePeerList(_map_server_info);
        }
    }

    if (new_peer) {
#ifdef ENABLE_SYNC_CHANGES
        SyncManager::Instance().onPeerAddedToCluster(id);
#endif
    }
    if (changed_peer) {
#ifdef ENABLE_SYNC_CHANGES
        // The endpoint or server group may have changed. Do not use the old peer until the new
        // configuration has passed a health check.
        SyncManager::Instance().removePeer(id);
#endif
    }

    auto origin_urls = buildOrginUrls(info);
    std::weak_ptr<ClusterManager> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self, id, origin_urls, revision]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        self->healthCheck(id, origin_urls, revision);
    });
}
    
void ClusterManager::removeMediaServer(const std::string &id) {
    bool same_group = false;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        auto it = _map_server_info.find(id);
        if (it != _map_server_info.end()) {
            GET_CONFIG(int, self_server_group_id, Cluster::kServerLocationId);
            same_group = it->second.serverGroupId == self_server_group_id;
            _map_server_info.erase(it);
        }
        _map_peer_url.erase(id);
        _map_peer_revision.erase(id);
        savePeerList(_map_server_info);
        DebugL << "Removed media server: " << id;
    }
#ifdef ENABLE_SYNC_CHANGES
    SyncManager::Instance().removePeer(id);
    if (same_group) {
        // Only peers of this server group share the ESC data synced here.
        SyncManager::Instance().onPeerRemovedFromCluster(id);
    }
#endif
}

void ClusterManager::healthCheck(const std::string &id, const std::string &origin_urls, uint64_t revision) {
    GET_CONFIG(string, kMediaServerId, General::kMediaServerId);
    if (id == kMediaServerId) {
        TraceL << "Skip health check for self node " << id;
        return;
    }
    weak_ptr<ClusterManager> weak_self = shared_from_this();
    Broadcast::HealthInvoker invoker = [weak_self, origin_urls, id, revision](const std::string& err, const int& idx) {
        auto self = weak_self.lock();
        if (!self) return;
        self->onHealthCheckResult(id, origin_urls, revision, err, idx);
    };

    NOTICE_EMIT(BroadcastHealthCheckMediaServiceArgs, Broadcast::kBroadcastHealthCheckMediaService, origin_urls, invoker);
}

void ClusterManager::onHealthCheckResult(const std::string &id, const std::string &origin_urls, uint64_t revision,
                                         const std::string &err, int idx) {
    if (!err.empty()) {
        bool was_healthy = false;
        {
            std::lock_guard<std::mutex> lck(_mtx);
            auto rev_it = _map_peer_revision.find(id);
            if (rev_it == _map_peer_revision.end() || rev_it->second != revision) {
                TraceL << "Ignore stale health check failure for peer " << id;
                return;
            }
            WarnL << "Health check failed for " << origin_urls << ": " << err;
            was_healthy = _map_peer_url.erase(id) > 0;
            if (was_healthy) {
                WarnL << "Peer " << id << " is unreachable, removing from active peer list";
#ifdef ENABLE_SYNC_CHANGES
                // Keep the cluster state and SyncManager transition ordered against a concurrent
                // peer update. SyncManager::removePeer only mutates its own peer map.
                SyncManager::Instance().removePeer(id);
#endif
            }
        }
        return;
    }

    std::vector<std::string> urls = split(origin_urls, ",");
    if (idx < 0 || static_cast<size_t>(idx) >= urls.size()) {
        WarnL << "Health check index out of range for " << origin_urls << ": " << idx;
        return;
    }

    auto url = urls[idx];
#ifdef ENABLE_SYNC_CHANGES
    GET_CONFIG(int, self_server_group_id, Cluster::kServerLocationId);
#endif
    {
        std::lock_guard<std::mutex> lck(_mtx);
        auto rev_it = _map_peer_revision.find(id);
        auto info_it = _map_server_info.find(id);
        if (rev_it == _map_peer_revision.end() || rev_it->second != revision || info_it == _map_server_info.end()) {
            TraceL << "Ignore stale health check success for peer " << id;
            return;
        }
        _map_peer_url[id] = url;
#ifdef ENABLE_SYNC_CHANGES
        // Apply the SyncManager transition before releasing _mtx so a configuration update cannot
        // invalidate this revision between the check above and the peer-map update.
        if (info_it->second.serverGroupId == self_server_group_id) {
            SyncManager::Instance().addPeer(id, url);
        } else {
            SyncManager::Instance().removePeer(id);
        }
#endif
    }
    DebugL << "Health check succeeded for " << url;
}

void ClusterManager::loadSavedMediaServerInfo() {
    GET_CONFIG(string, peer_list_json, Peer::kPeerList);
    if (peer_list_json.empty() || peer_list_json == "[]") return;

    Json::Value list;
    if (!StrJsonUtils::readJsonString(peer_list_json, list) || !list.isArray()) {
        WarnL << "ClusterManager: failed to parse persisted peer list";
        return;
    }

    std::weak_ptr<ClusterManager> weak_self = shared_from_this();
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
    std::vector<std::tuple<std::string, std::string, uint64_t> > peers;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        for (const auto &pr : _map_server_info) {
            if (pr.first == mINI::Instance()[General::kMediaServerId]) {
                // Skip self node
                continue;
            }
            // Check all peers (both healthy and unhealthy) to detect connection changes
            auto rev_it = _map_peer_revision.find(pr.first);
            if (rev_it != _map_peer_revision.end()) {
                peers.emplace_back(pr.first, buildOrginUrls(pr.second), rev_it->second);
            }
        }
    }
    for (const auto &item : peers) {
        string id = std::get<0>(item);
        string origin_urls = std::get<1>(item);
        uint64_t revision = std::get<2>(item);
        std::weak_ptr<ClusterManager> weak_self = shared_from_this();
        WorkThreadPool::Instance().getPoller()->async([weak_self, id, origin_urls, revision]() {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            self->healthCheck(id, origin_urls, revision);
        });
    }
}

} // namespace managerkit

