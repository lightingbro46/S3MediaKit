#ifdef ENABLE_SYNC_CHANGES

#include "WebApi.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "WebApiErrCode.h"
#include "Extension/SyncManager.h"
#include "ApiWorkDispatcher.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
 
namespace managerkit {

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

        dispatchApiWork(sender, headerOut, val, invoker, [cursors, ack_cursors, limit](Json::Value &val) {
            // Save ack_cursors to local db for prune old logs later
            if (!ack_cursors.empty() && ack_cursors.isArray()) {
                SyncManager::Instance().recordRelayAck(ack_cursors);
            }

            // Parse cursors from request
            Json::Value log_rows = Json::arrayValue;
            if (!cursors.empty() && cursors.isArray()) {
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
