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

    Json::Value toJson() const {
        Json::Value v;
        v["peer_guid"] = peer_guid;
        v["db_guid"] = db_guid;
        v["sequence"] = sequence;
        v["timestamp"] = timestamp;
        v["tran_guid"] = tran_guid;
        v["tran_data"] = tran_data;
        v["tran_type"] = tran_type;
        v["timestamp_hi"] = timestamp_hi;
        return v;
    }

    static TransactionLog fromJson(const Json::Value &v) {
        TransactionLog log;
        log.peer_guid = v["peer_guid"].asString();
        log.db_guid = v["db_guid"].asString();
        log.sequence = v["sequence"].asInt();
        log.timestamp = v["timestamp"].asInt64();
        log.tran_guid = v["tran_guid"].asString();
        log.tran_data = v["tran_data"].asString();
        log.tran_type = v["tran_type"].asInt();
        log.timestamp_hi = v["timestamp_hi"].asInt();
        return log;
    }
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

    bool removeByPeerGuidAndDbGuid(const std::string &peer_guid, const std::string &db_guid, int less_than_seq = 0) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "peer_guid = ? AND db_guid = ?";
        whereParams.push_back(peer_guid);
        whereParams.push_back(db_guid);

        if (less_than_seq > 0) {
            whereClause << " AND sequence < ?";
            whereParams.push_back(std::to_string(less_than_seq));
        }

        auto query = toolkit::QueryBuilder()
                            .deleteFrom(EntityTraits<TransactionLog>::tableName())
                            .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }

    // Gossip relay: fetch log entries for all peers in the cursor map.
    // cursors: key = "peer_guid|db_guid", value = since_seq (0 = from beginning).
    // Returns rows across all known peers, ordered by (peer_guid, sequence).
    std::vector<TransactionLog> findAllSince(const std::vector<TransactionSequence> &cursors, int limit = 100) {
        if (cursors.empty()) return {};

        std::ostringstream whereClause;
        std::vector<std::string> whereParams;
        bool first = true;

        for (const auto &c : cursors) {
            if (!first) whereClause << " OR ";
            whereClause << "(peer_guid = ? AND db_guid = ? AND sequence > ?)";
            whereParams.push_back(c.peer_guid);
            whereParams.push_back(c.db_guid);
            whereParams.push_back(std::to_string(c.sequence));
            first = false;
        }

        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<TransactionLog>::getColumns())
                        .from(EntityTraits<TransactionLog>::tableName())
                        .where(whereClause.str(), whereParams)
                        .orderBy("peer_guid ASC, sequence ASC");
        if (limit > 0) query.limit(limit);

        auto rows = _executor->executeRaw(query);
        std::vector<TransactionLog> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<TransactionLog>::fromRow(row));
        }
        return ret;
    }

    std::vector<TransactionLog> findAllSinceWithTxn(const std::vector<TransactionSequence> &cursors, int limit, toolkit::SqliteTransaction::Ptr txn) {
        if (cursors.empty()) return {};

        std::ostringstream whereClause;
        std::vector<std::string> whereParams;
        bool first = true;

        for (const auto &c : cursors) {
            if (!first) whereClause << " OR ";
            whereClause << "(peer_guid = ? AND db_guid = ? AND sequence > ?)";
            whereParams.push_back(c.peer_guid);
            whereParams.push_back(c.db_guid);
            whereParams.push_back(std::to_string(c.sequence));
            first = false;
        }

        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<TransactionLog>::getColumns())
                        .from(EntityTraits<TransactionLog>::tableName())
                        .where(whereClause.str(), whereParams)
                        .orderBy("peer_guid ASC, sequence ASC");
        if (limit > 0) query.limit(limit);

        auto rows = _executor->executeRawWithTxn(txn, query);
        std::vector<TransactionLog> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<TransactionLog>::fromRow(row));
        }
        return ret;
    }

    // Dedup check: returns true if an entry with this tran_guid already exists.
    bool existsByTranGuid(const std::string &tran_guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "tran_guid = ?";
        whereParams.push_back(tran_guid);

        auto query = toolkit::QueryBuilder()
                        .select({"tran_guid"})
                        .from(EntityTraits<TransactionLog>::tableName())
                        .where(whereClause.str(), whereParams)
                        .limit(1);
        return !_executor->executeRaw(query).empty();
    }

};

class TransactionLogImp : public TransactionLogRepository {
public:
    using Ptr = std::shared_ptr<TransactionLogImp>;

    TransactionLogImp() : TransactionLogRepository() {
        _seq_impl = std::make_shared<TransactionSequenceImp>();
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

    // Transaction-aware variant: insert the log entry within an existing transaction.
    // The sequence update is intentionally kept outside the transaction (it's metadata).
    void addWithTxn(TransactionLog &entity, toolkit::SqliteTransaction::Ptr txn) {
        if (saveWithTxn(entity, txn)) {
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

    /**
     * Delete transaction_log entries for this node (peer_guid = self_peer_guid)
     * with sequence <= safe_seq.  Called by SyncManager after computing the
     * watermark across all active peers.
     */
    void pruneAckedLogs(const std::string &peer_guid,
                        const std::string &db_guid,
                        int safe_seq) {
        if (safe_seq <= 0) return;
        // removeByPeerGuid deletes WHERE sequence < less_than_seq, so pass safe_seq + 1 to include safe_seq itself.
        removeByPeerGuidAndDbGuid(peer_guid, db_guid, safe_seq + 1);
    }

private:
    TransactionSequenceImp::Ptr _seq_impl;
};

} // namespace managerkit

#endif // STORAGE_TRANSACTION_LOG_H