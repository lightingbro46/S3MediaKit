#ifndef STORAGE_TRANSACTION_SEQUENCE_H
#define STORAGE_TRANSACTION_SEQUENCE_H

#include <string>
#include "DbStorage.h"

namespace managerkit {

struct TransactionSequence {
    std::string peer_guid;
    std::string db_guid;
    int sequence;
};

DECLARE_ENTITY_NO_PK(TransactionSequence, "transaction_sequence",
    &TransactionSequence::peer_guid, "peer_guid", 
    &TransactionSequence::db_guid, "db_guid", 
    &TransactionSequence::sequence, "sequence"
)

class TransactionSequenceRepository : public SqliteRepository<TransactionSequence> {
public:
    TransactionSequenceRepository() : SqliteRepository<TransactionSequence>(Database::kEdgeStorageControllerDb) {}

    std::vector<TransactionSequence> findByPeerIdAndDbGuid(const TransactionSequence &seq) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "peer_guid = ? AND db_guid = ?";
        whereParams.push_back(seq.peer_guid);
        whereParams.push_back(seq.db_guid);

        auto query = toolkit::QueryBuilder()
                            .select(EntityTraits<TransactionSequence>::getColumns())
                            .from(EntityTraits<TransactionSequence>::tableName())
                            .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<TransactionSequence> ret;
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<TransactionSequence>::fromRow(row));
        }
        return ret;
    }

    std::vector<TransactionSequence> findByPeerId(const TransactionSequence &seq) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "peer_guid = ?";
        whereParams.push_back(seq.peer_guid);

        auto query = toolkit::QueryBuilder()
                            .select(EntityTraits<TransactionSequence>::getColumns())
                            .from(EntityTraits<TransactionSequence>::tableName())
                            .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<TransactionSequence> ret;
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<TransactionSequence>::fromRow(row));
        }
        return ret;
    }

    bool updateByPeerGuidAndDbGuid(const TransactionSequence &seq) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "peer_guid = ? AND db_guid = ?";
        whereParams.push_back(seq.peer_guid);
        whereParams.push_back(seq.db_guid);

        auto query = toolkit::QueryBuilder()
                            .update(EntityTraits<TransactionSequence>::tableName())
                            .set({{"sequence", std::to_string(seq.sequence)}})
                            .where(whereClause.str(), whereParams);
        return _executor->execDML(query) > 0;
    }

    int findMinSequence() {
        auto query = toolkit::QueryBuilder()
                            .select({"MIN(sequence)"})
                            .from(EntityTraits<TransactionSequence>::tableName());
        auto rows = _executor->executeRaw(query);
        if (rows.size() > 0 && rows[0].size() > 0) {
            return std::stoi(rows[0][0]);
        }
        return 0;
    }
};

class TransactionSequenceImp : public TransactionSequenceRepository {
public:
    using Ptr = std::shared_ptr<TransactionSequenceImp>;
    TransactionSequenceImp() : TransactionSequenceRepository() {}

    void add(TransactionSequence &entity) {
        auto entities = findByPeerIdAndDbGuid(entity);
        if (entities.empty()) {
            save(entity);
        } else {
            updateByPeerGuidAndDbGuid(entity);
        }
    }

    int findSeqByPeerIdAndDbGuid(const std::string &peer_guid, const std::string &db_guid) {
        TransactionSequence entity;
        entity.peer_guid = peer_guid;
        entity.db_guid = db_guid;
        auto entities = findByPeerIdAndDbGuid(entity);
        if (entities.size() > 0) {
            return entities[0].sequence;
        }
        return 0;
    }

    std::string findDbGuidByPeerId(const std::string &peer_guid) {
        TransactionSequence entity;
        entity.peer_guid = peer_guid;
        auto entities = findByPeerId(entity); // findByPeerIdAndDbGuid would filter on db_guid='' and never match
        if (entities.size() > 0) {
            return entities[0].db_guid;
        }
        return "";
    }
};

} // namespace managerkit

#endif // STORAGE_TRANSACTION_SEQUENCE_H