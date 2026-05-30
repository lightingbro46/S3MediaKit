#ifndef STORAGE_TRANSACTION_LOG_H
#define STORAGE_TRANSACTION_LOG_H

#include <string>
#include <json/json.h>
#include "DbStorage.h"
#include "TransactionSequence.h"

namespace managerkit {

enum class TranType : uint8_t {
    DataMutation  = 0,   // insert/update/delete thông thường
};

#define TRAN_DATA_OP_UPSERT "UPSERT"
#define TRAN_DATA_OP_UPSERT_BATCH "UPSERT_BATCH"
#define TRAN_DATA_OP_DELETE "DELETE"

/**
 * TransactionLog is the data structure used to store the transaction log of database.
 * It contains the peer_id and db_guid of the media server, the sequence number of the transaction, 
 * the timestamp of the transaction, the guid of the transaction, the data of the transaction (in JSON string), 
 * the type of the transaction, and a high-precision timestamp for ordering transactions with same sequence number.
 * Format of tran_data JSON string: {"table":"...","op":"UPSERT|DELETE","payload":{...}}
 */
struct TransactionLog {
    std::string peer_guid;
    std::string db_guid;
    int sequence;
    int64_t timestamp;
    std::string tran_guid;
    std::string tran_data;
    int tran_type;
    int timestamp_hi;
};

DECLARE_ENTITY_NO_PK(TransactionLog, "transaction_log",
    &TransactionLog::peer_guid, "peer_guid",
    &TransactionLog::db_guid, "db_guid",
    &TransactionLog::sequence, "sequence",
    &TransactionLog::timestamp, "timestamp",
    &TransactionLog::tran_guid, "tran_guid",
    &TransactionLog::tran_data, "tran_data",
    &TransactionLog::tran_type, "tran_type",
    &TransactionLog::timestamp_hi, "timestamp_hi"
)

class TransactionLogRepository : public SqliteRepository<TransactionLog> {
public:
    TransactionLogRepository() : SqliteRepository<TransactionLog>(Database::kEdgeStorageControllerDb) {}

    std::vector<TransactionLog> findByPeerGuid(const TransactionLog &log, int since_seq = 0, int limit = 100) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "peer_guid = ?";
        whereParams.push_back(log.peer_guid);

        if (since_seq > 0) {
            whereClause << " AND sequence > ?";
            whereParams.push_back(std::to_string(since_seq));
        }

        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<TransactionLog>::getColumns())
                        .from(EntityTraits<TransactionLog>::tableName())
                        .where(whereClause.str(), whereParams)
                        .orderBy("sequence ASC");

        if (limit > 0) {
            query.limit(limit);
        }
        auto rows = _executor->executeRaw(query);
        std::vector<TransactionLog> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<TransactionLog>::fromRow(row));
        }
        return ret;
    }

    bool removeByPeerGuid(const std::string &peer_guid, int less_than_seq = 0) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "peer_guid = ?";
        whereParams.push_back(peer_guid);

        if (less_than_seq > 0) {
            whereClause << " AND sequence < ?";
            whereParams.push_back(std::to_string(less_than_seq));
        }

        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<TransactionLog>::tableName())
                            .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }

};

class TransactionLogImp : public TransactionLogRepository {
public:
    using Ptr = std::shared_ptr<TransactionLogImp>;

    TransactionLogImp() : TransactionLogRepository() {
        _seq_impl = std::make_shared<TransactionSequenceImp>();
        loadSelfInfo();
    }

    void add(TransactionLog &entity) {
        if (save(entity)) {
            // update sequence
            TransactionSequence seq;
            seq.peer_guid = entity.peer_guid;
            seq.db_guid = entity.db_guid;
            seq.sequence = entity.sequence;
            _seq_impl->add(seq);
        }
    }

    // todo: remove log

    std::vector<TransactionLog> findSinceSeq(const std::string &peer_guid, const std::string &db_guid, int since_seq = 0, int limit = 100) {
        TransactionLog log;
        log.peer_guid = peer_guid;
        log.db_guid = db_guid;
        return findByPeerGuid(log, since_seq, limit);
    }

    void appendLocalDataMutation(const std::string &table, const std::string &op, const Json::Value &payload);

private:
    void loadSelfInfo();

private:
    TransactionSequenceImp::Ptr _seq_impl;
    std::string _self_node_id; 
    std::string _self_db_guid;
};

} // namespace managerkit

#endif // STORAGE_TRANSACTION_LOG_H