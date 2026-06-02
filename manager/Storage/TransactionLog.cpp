#include "TransactionLog.h"
#include "Common/StrUtil.h"
#include "Common/config.h"
#include "Extension/Resource.h"

using namespace std;
using namespace toolkit;
using namespace mediakit; 

namespace managerkit {

void TransactionLogImp::appendLocalDataMutation(const std::string &table, const std::string &op, const Json::Value &payload) {
    auto self_node_id = ResourceManager::Instance().getSelfNodeId();
    auto self_db_guid = ResourceManager::Instance().getSelfDbGuid();

    auto current_seq = _seq_impl->findSeqByPeerIdAndDbGuid(self_node_id, self_db_guid);

    TransactionLog log;
    log.peer_guid = self_node_id;
    log.db_guid = self_db_guid;
    log.sequence = current_seq + 1;
    log.timestamp = static_cast<int64_t>(toolkit::getCurrentMillisecond(true));
    log.timestamp_hi = 0; // default to 0, can be set to 1 to indicate this log wins in conflict resolution regardless of timestamp
    log.tran_guid = toolkit::makeUuidStr();
    log.tran_type = static_cast<int>(TranType::DataMutation);

    Json::Value tran_data_json;
    tran_data_json["table"] = table;
    tran_data_json["op"]    = op;
    tran_data_json["payload"] = payload;
    log.tran_data = StrJsonUtils::writeJsonString(tran_data_json);

    add(log);
}

} // namespace managerkit