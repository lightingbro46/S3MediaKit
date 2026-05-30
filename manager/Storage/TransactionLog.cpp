#include "TransactionLog.h"
#include "Common/StrUtil.h"
#include "Common/config.h"
#include "MiscData.h"

using namespace std;
using namespace toolkit;
using namespace mediakit; 

namespace managerkit {

void TransactionLogImp::loadSelfInfo() {
    if (_self_node_id.empty()) {
        GET_CONFIG(std::string, mediaServerId, mediakit::General::kMediaServerId);
        _self_node_id = mediaServerId;
    }
    if (_self_db_guid.empty()) {
        auto imp = std::make_shared<MiscDataImp>();
        auto ret = imp->findByKey(MISC_DATA_DB_INSTANCE_ID_KEY);
        if (!ret.empty()) {
            _self_db_guid = ret[0].value;
        }
    }
    CHECK(!_self_node_id.empty() && !_self_db_guid.empty());
}

void TransactionLogImp::appendLocalDataMutation(const std::string &table, const std::string &op, const Json::Value &payload) {
    auto current_seq = _seq_impl->findSeqByPeerIdAndDbGuid(_self_node_id, _self_db_guid);

    TransactionLog log;
    log.peer_guid = _self_node_id;
    log.db_guid = _self_db_guid;
    log.sequence = current_seq + 1;
    log.timestamp = static_cast<int64_t>(toolkit::getCurrentMillisecond());
    log.timestamp_hi = 0; // todo
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