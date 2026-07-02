#include "WebApi.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "WebApiErrCode.h"
#include "Extension/SyncManager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
 
namespace managerkit {

void registerSyncDbApis() {
    // Register the Web API sync database endpoints here
    api_regist("/media/esc/sync/changes", [](API_ARGS_JSON_ASYNC) {
        CHECK_ARGS_("cursors", "ack_cursors", "limit", "peer", "db");

        string peer_id      = allArgs["peer"];
        string db_guid      = allArgs["db"];
        int limit           = allArgs["limit"];
        Json::Value cursors = allArgs["cursors"];
        Json::Value ack_cursors = allArgs["ack_cursors"];

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
        invoker(200, headerOut, val.toStyledString());
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

        auto snap = SyncManager::Instance().buildLocalSnapshot();
        // if (snap.empty()) {
        //     RETURN_API_RESPONSE(ApiErrCode::CODE_SNAPSHOT_EMPTY, "Snapshot is empty");
        //     return;
        // }
        Json::Value snap_json = SnapshotBuilder::serialize(snap);
        val["data"] = snap_json;
        invoker(200, headerOut, val.toStyledString());
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
        
        val["data"] = SyncManager::Instance().getMiscData();
        invoker(200, headerOut, val.toStyledString());
    });

    DebugL << "Sync DB APIs registered";
}

} // namespace managerkit
