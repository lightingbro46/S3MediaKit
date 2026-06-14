#ifndef STORAGE_PEER_ACK_LOG_H
#define STORAGE_PEER_ACK_LOG_H

#include <string>
#include <climits>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include "DbStorage.h"
#include "Common/config.h"
#include "json/json.h"

namespace managerkit {

/**
 * PeerAckLog records how far each remote peer has successfully pulled from this node.
 *
 *   peer_guid  — the remote peer's node ID (the puller)
 *   db_guid    — this node's own db_guid (the pulled-from)
 *   acked_seq  — the highest sequence number the peer has already received
 *   updated_at — last update timestamp (ms)
 *
 * GC logic: once all active peers have acked up to seq N, transaction_log
 * entries with sequence <= N can be safely deleted.
 */
struct PeerAckLog {
    std::string peer_guid;
    std::string db_guid;
    std::string src_peer_guid;
    std::string src_db_guid;
    int         acked_seq;
    int64_t     updated_at;

    Json::Value toJson() const {
        Json::Value v;
        v["peer_guid"] = peer_guid;
        v["db_guid"]   = db_guid;
        v["src_peer_guid"] = src_peer_guid;
        v["src_db_guid"] = src_db_guid;
        v["acked_seq"] = acked_seq;
        v["updated_at"] = updated_at;
        return v;
    }

    static PeerAckLog fromJson(const Json::Value &v) {
        PeerAckLog ack_log;
        ack_log.peer_guid = v["peer_guid"].asString();
        ack_log.db_guid = v["db_guid"].asString();
        ack_log.src_peer_guid = v["src_peer_guid"].asString();
        ack_log.src_db_guid = v["src_db_guid"].asString();
        ack_log.acked_seq = v["acked_seq"].asInt();
        ack_log.updated_at = v["updated_at"].asInt64();
        return ack_log;
    }
};

DECLARE_ENTITY(PeerAckLog, "transaction_peer_ack_log",
    MAKE_PK("peer_guid", "src_peer_guid"),
    &PeerAckLog::peer_guid,  "peer_guid",
    &PeerAckLog::db_guid,    "db_guid",
    &PeerAckLog::src_peer_guid,  "src_peer_guid",
    &PeerAckLog::src_db_guid,    "src_db_guid",
    &PeerAckLog::acked_seq,  "acked_seq",
    &PeerAckLog::updated_at, "updated_at"
)

class PeerAckLogRepository : public SqliteRepository<PeerAckLog> {
public:
    PeerAckLogRepository() : SqliteRepository<PeerAckLog>(Database::kEdgeStorageControllerDb) {}
    
    /**
     * Returns all ack logs for the given peer_guid (puller) and src_peer_guid (pulled-from).  Should be at most one entry, but return vector for flexibility.
     */
    std::vector<PeerAckLog> findByPeerGuidAndSrcPeerGuid(const std::string &peer_guid, const std::string &src_peer_guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "peer_guid = ? AND src_peer_guid = ?";
        whereParams.push_back(peer_guid);
        whereParams.push_back(src_peer_guid);

        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<PeerAckLog>::getColumns())
                        .from(EntityTraits<PeerAckLog>::tableName())
                        .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);

        std::vector<PeerAckLog> ack_logs;
        for (const auto &row : rows) {
            ack_logs.push_back(EntityTraits<PeerAckLog>::fromRow(row));
        }
        return ack_logs;
    }

    /**
     * Returns the minimum acked_seq among all active peers for this db_guid.
     * If any active peer has never acked (no entry), returns 0 (cannot prune yet).
     */
    bool removeByPeerGuid(const std::string &peer_guid) {
        auto query = toolkit::QueryBuilder()
                        .deleteFrom(EntityTraits<PeerAckLog>::tableName())
                        .where("peer_guid = ?", {peer_guid});
        return _executor->execDML(query) > 0;
    }

    /**
     * Returns the minimum acked_seq among all active peers for this db_guid.
     * If any active peer has never acked (no entry), returns 0 (cannot prune yet).
     */
    struct AckedSeq {
        std::string peer_guid;
        std::string db_guid;
        int sequence;
    };
    std::vector<AckedSeq> findMinAckedSeq(const std::unordered_set<std::string> &active_peers) {
        if (active_peers.empty()) return {};

        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "peer_guid IN (";
        bool first = true;
        for (const auto &peer_guid : active_peers) {
            if (!first) whereClause << ",";
            whereClause << "?";
            whereParams.push_back(peer_guid);
            first = false;
        }
        whereClause << ")";

        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<PeerAckLog>::getColumns())
                        .from(EntityTraits<PeerAckLog>::tableName())
                        .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);

        // Group by (src_peer_guid, src_db_guid) → { peer_guid → acked_seq }
        struct SrcKey {
            std::string src_peer_guid;
            std::string src_db_guid;
            bool operator<(const SrcKey &o) const {
                if (src_peer_guid != o.src_peer_guid) return src_peer_guid < o.src_peer_guid;
                return src_db_guid < o.src_db_guid;
            }
        };
        std::map<SrcKey, std::unordered_map<std::string, int>> groups;
        for (const auto &row : rows) {
            auto entry = EntityTraits<PeerAckLog>::fromRow(row);
            SrcKey key = { entry.src_peer_guid, entry.src_db_guid };
            groups[key][entry.peer_guid] = entry.acked_seq;
        }

        std::vector<AckedSeq> result;
        for (const auto &gp : groups) {
            const auto &peer_acks = gp.second;
            int min_seq = INT_MAX;
            bool missing = false;
            for (const auto &ap : active_peers) {
                auto it = peer_acks.find(ap);
                if (it == peer_acks.end()) { missing = true; break; }
                if (it->second < min_seq) min_seq = it->second;
            }
            if (missing || min_seq <= 0 || min_seq == INT_MAX) continue;

            AckedSeq aseq;
            aseq.peer_guid = gp.first.src_peer_guid;  // used as source peer for pruneAckedLogs
            aseq.db_guid   = gp.first.src_db_guid;
            aseq.sequence  = min_seq;
            result.push_back(aseq);
        }
        return result;
    }

    std::vector<PeerAckLog> findAll() {
        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<PeerAckLog>::getColumns())
                        .from(EntityTraits<PeerAckLog>::tableName());
        auto rows = _executor->executeRaw(query);

        std::vector<PeerAckLog> ack_logs;
        for (const auto &row : rows) {
            ack_logs.push_back(EntityTraits<PeerAckLog>::fromRow(row));
        }
        return ack_logs;
    }
};

class PeerAckLogImp : public PeerAckLogRepository {
public:
    using Ptr = std::shared_ptr<PeerAckLogImp>;

    PeerAckLogImp() : PeerAckLogRepository() {}

    /**
     * Upsert the acked sequence for a given peer_guid and src_peer_guid. If no existing entry, insert a new one; if entry exists, update it.
     */
    void add(PeerAckLog &entity) {
        if (entity.peer_guid.empty() || entity.src_peer_guid.empty() || entity.acked_seq <= 0) return;
        static std::mutex s_add_mtx;
        std::lock_guard<std::mutex> lk(s_add_mtx);
        auto ret = findByPeerGuidAndSrcPeerGuid(entity.peer_guid, entity.src_peer_guid);
        if (ret.empty()) {
            // No existing entry, insert new one
            save(entity, true);
            return;
        }
        auto &existing = ret[0];
        if (entity.acked_seq > existing.acked_seq && entity.updated_at > existing.updated_at) {
            // Update existing entry with higher acked_seq
            updateById(entity);
        } else {
            WarnL << "Not updating ack log for peer=" << entity.peer_guid
                  << " src_peer=" << entity.src_peer_guid
                  << " because existing acked_seq=" << existing.acked_seq
                  << " is higher than new acked_seq=" << entity.acked_seq;
        }
    }
};

} // namespace managerkit

#endif // STORAGE_PEER_ACK_LOG_H
