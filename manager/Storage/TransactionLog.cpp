#include "TransactionLog.h"
#include "Common/StrUtil.h"
#include "Common/config.h"
#include "Extension/Resource.h"
#ifdef ENABLE_SYNC_CHANGES
#include "Extension/SyncManager.h"
#endif
#include <mutex>

using namespace std;
using namespace toolkit;
using namespace mediakit; 

namespace managerkit {

void TransactionLogImp::appendLocalDataMutation(const std::string &table, const std::string &op, const Json::Value &payload) {
#ifndef ENABLE_SYNC_CHANGES
    (void)table;
    (void)op;
    (void)payload;
    return;
#else
    // Serialize all callers (across every TransactionLogImp instance) to prevent
    // a read-modify-write race where two concurrent calls both read the same
    // current_seq, compute the same next sequence, and then one INSERT succeeds
    // while the other hits the UNIQUE constraint on (peer_guid, db_guid, sequence).
    static std::mutex s_append_mtx;
    std::lock_guard<std::mutex> lk(s_append_mtx);

    auto self_node_id = ResourceManager::Instance().getSelfNodeId();
    auto self_db_guid = ResourceManager::Instance().getSelfDbGuid();

    auto current_seq = _seq_impl->findSeqByPeerIdAndDbGuid(self_node_id, self_db_guid);

    TransactionLog log;
    log.peer_guid = self_node_id;
    log.db_guid = self_db_guid;
    log.sequence = current_seq + 1;
    log.timestamp = static_cast<int64_t>(toolkit::getCurrentMillisecond(true));
    try {
        // Records the LWW versions of the touched rows; may move the timestamp forward so this
        // write is newer than any version this node already accepted for those rows.
        log.timestamp = SyncManager::Instance().stampLocalMutation(table, op, payload, log.timestamp, self_node_id);
    } catch (const std::exception &ex) {
        WarnL << "Failed to record row versions for local " << op << " on " << table << ": " << ex.what();
    }
    log.timestamp_hi = 0; // audit flag, only set for remote entries (1 = conflict winner, 2 = stale)
    log.tran_guid = toolkit::makeUuidStr();
    log.tran_type = static_cast<int>(TranType::DataMutation);

    Json::Value tran_data_json;
    tran_data_json["table"] = table;
    tran_data_json["op"]    = op;
    tran_data_json["payload"] = payload;
    log.tran_data = StrJsonUtils::writeJsonString(tran_data_json);

    add(log);
#endif // ENABLE_SYNC_CHANGES
}

} // namespace managerkit