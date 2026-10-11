#ifdef ENABLE_SYNC_CHANGES

#include <algorithm>
#include "WebApi.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "WebApiErrCode.h"
#include "Extension/SyncManager.h"
#include "Server/ClusterManager.h"
#include "ApiWorkDispatcher.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
 
namespace managerkit {

// Sync data is served only to the media servers the service API lists in this server group (the ones
// SyncManager pulls from). A server removed from the cluster that still runs must neither read the
// cluster's data nor record acks; it is refused as soon as this node's list no longer has it.
static bool isSyncMember(const string &peer_id) {
    if (peer_id.empty()) {
        return false;
    }
    auto &cluster = ClusterManager::Instance();
    auto ids = cluster.getMediaServerIds();
    if (std::find(ids.begin(), ids.end(), peer_id) == ids.end()) {
        return false;
    }
    GET_CONFIG(int, self_group_id, Cluster::kServerLocationId);
    return cluster.getMediaServer(peer_id).serverGroupId == self_group_id;
}

// All sync endpoints do blocking SQLite work (log scan, snapshot build, ack upsert): validation stays on
// the request poller, the DB work runs on the API worker pool (see ApiWorkDispatcher.h).
void registerSyncDbApis() {
    // Register the Web API sync database endpoints here
    api_regist("/media/esc/sync/changes", [](API_ARGS_JSON_ASYNC) {
        // Array parameters must not go through allArgs[] / CHECK_ARGS_: they convert via
        // Json::Value::asString(), which throws "Type is not convertible to string" for arrays.
        CHECK_ARGS_("limit", "peer", "db");
        const Json::Value &body = allArgs.getArgs();
        if (!body.isMember("cursors") || !body.isMember("ack_cursors")) {
            throw InvalidArgsException("Required parameter missed: cursors, ack_cursors", ApiErrCode::CODE_INVALID_ARGS);
        }

        string peer_id      = allArgs["peer"];
        string db_guid      = allArgs["db"];
        int limit           = allArgs["limit"];
        Json::Value cursors = body["cursors"];
        Json::Value ack_cursors = body["ack_cursors"];
        if (!isSyncMember(peer_id)) {
            WarnL << "Sync changes refused: media server " << peer_id << " is not a member of this cluster";
            RETURN_API_RESPONSE(ApiErrCode::CODE_PERMISSION_DENIED, "Media server is not a member of this cluster");
            return;
        }

        dispatchApiWork(sender, headerOut, val, invoker, [cursors, ack_cursors, limit](Json::Value &val) {
            // Save ack_cursors to local db for prune old logs later
            if (!ack_cursors.empty() && ack_cursors.isArray()) {
                SyncManager::Instance().recordRelayAck(ack_cursors);
            }

            // Parse cursors from request. An empty map is valid: a node that has not written anything yet
            // knows no source, getCurrentCursors() then starts it from 0 for every source known here.
            Json::Value log_rows = Json::arrayValue;
            if (cursors.isArray()) {
                log_rows = SyncManager::Instance().getCurrentCursors(cursors, limit);
            }
            val["data"] = log_rows;
        }, nullptr, apiWorkLimit("sync.changes"));
    });

    api_regist("/media/esc/sync/snapshot", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("peer");
        string peer_id = allArgs["peer"];

        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (peer_id != mediaServerId) {
            // todo: forward request to other media server if node_id is not current media server id
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }
        // "peer" names this server; the caller is "mediaServerId" (added to every hook request).
        string requester = allArgs["mediaServerId"];
        if (!isSyncMember(requester)) {
            WarnL << "Sync snapshot refused: media server " << requester << " is not a member of this cluster";
            RETURN_API_RESPONSE(ApiErrCode::CODE_PERMISSION_DENIED, "Media server is not a member of this cluster");
            return;
        }

        dispatchApiWork(sender, headerOut, val, invoker, [](Json::Value &val) {
            // A node whose own bootstrap is not complete must not seed another node: its data may
            // be empty or partial. The caller then tries another peer.
            val["bootstrapped"] = SyncManager::Instance().isBootstrapped();
            if (!val["bootstrapped"].asBool()) {
                return;
            }
            auto snap = SyncManager::Instance().buildLocalSnapshot();
            // if (snap.empty()) {
            //     RETURN_API_RESPONSE(ApiErrCode::CODE_SNAPSHOT_EMPTY, "Snapshot is empty");
            //     return;
            // }
            val["data"] = SnapshotBuilder::serialize(snap);
        }, nullptr, apiWorkLimit("sync.snapshot"));
    });

    api_regist("/media/esc/sync/misc", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("peer");
        string peer_id = allArgs["peer"];

        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (peer_id != mediaServerId) {
            // todo: forward request to other media server if node_id is not current media server id
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }
        
        dispatchApiWork(sender, headerOut, val, invoker, [](Json::Value &val) {
            val["data"] = SyncManager::Instance().getMiscData();
        });
    });

    DebugL << "Sync DB APIs registered";
}

} // namespace managerkit

#endif // ENABLE_SYNC_CHANGES
