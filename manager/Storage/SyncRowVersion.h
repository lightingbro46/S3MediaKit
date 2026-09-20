#ifndef STORAGE_SYNC_ROW_VERSION_H
#define STORAGE_SYNC_ROW_VERSION_H

#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"

namespace managerkit {

/**
 * Last accepted version of a replicated ESC row (see migration 09_sync_row_version.sql).
 * A version is the pair (ts_ms, peer_guid) of the transaction that wrote the row.
 */
struct SyncRowVersion {
    std::string table_name;
    std::string row_key;
    std::string group_key;
    int64_t ts_ms = 0;
    std::string peer_guid;
    int deleted = 0;

    Json::Value toJson() const {
        Json::Value v;
        v["table_name"] = table_name;
        v["row_key"]    = row_key;
        v["group_key"]  = group_key;
        v["ts_ms"]      = static_cast<Json::Int64>(ts_ms);
        v["peer_guid"]  = peer_guid;
        v["deleted"]    = deleted;
        return v;
    }

    static SyncRowVersion fromJson(const Json::Value &v) {
        SyncRowVersion r;
        r.table_name = v["table_name"].asString();
        r.row_key    = v["row_key"].asString();
        r.group_key  = v["group_key"].asString();
        r.ts_ms      = v["ts_ms"].asInt64();
        r.peer_guid  = v["peer_guid"].asString();
        r.deleted    = v["deleted"].asInt();
        return r;
    }
};

DECLARE_ENTITY(SyncRowVersion, "sync_row_version",
    std::vector<std::string>({"table_name", "row_key"}),
    &SyncRowVersion::table_name, "table_name",
    &SyncRowVersion::row_key, "row_key",
    &SyncRowVersion::group_key, "group_key",
    &SyncRowVersion::ts_ms, "ts_ms",
    &SyncRowVersion::peer_guid, "peer_guid",
    &SyncRowVersion::deleted, "deleted"
)

class SyncRowVersionRepository : public SqliteRepository<SyncRowVersion> {
public:
    SyncRowVersionRepository() : SqliteRepository<SyncRowVersion>(Database::kEdgeStorageControllerDb) {}

    std::vector<SyncRowVersion> findAllWithTxn(toolkit::SqliteTransaction::Ptr txn) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<SyncRowVersion>::getColumns())
                         .from(EntityTraits<SyncRowVersion>::tableName());
        return toRows(executeRawWithTxn(txn, query));
    }

protected:
    std::vector<SyncRowVersion> findByKeyWithTxn(const std::string &table_name, const std::string &row_key,
                                                 toolkit::SqliteTransaction::Ptr txn) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<SyncRowVersion>::getColumns())
                         .from(EntityTraits<SyncRowVersion>::tableName())
                         .where("table_name = ? AND row_key = ?", {table_name, row_key});
        return toRows(executeRawWithTxn(txn, query));
    }

    // Live (not deleted) rows of a group written strictly after version (ts_ms, peer_guid).
    std::vector<SyncRowVersion> findLiveInGroupNewerThanWithTxn(const std::string &table_name, const std::string &group_key,
                                                                int64_t ts_ms, const std::string &peer_guid,
                                                                toolkit::SqliteTransaction::Ptr txn) {
        auto ts = std::to_string(ts_ms);
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<SyncRowVersion>::getColumns())
                         .from(EntityTraits<SyncRowVersion>::tableName())
                         .where("table_name = ? AND group_key = ? AND deleted = 0 AND (ts_ms > ? OR (ts_ms = ? AND peer_guid > ?))",
                                {table_name, group_key, ts, ts, peer_guid});
        return toRows(executeRawWithTxn(txn, query));
    }

    bool removeTombstonesBeforeWithTxn(int64_t before_ts_ms, toolkit::SqliteTransaction::Ptr txn) {
        auto query = toolkit::QueryBuilder()
                         .deleteFrom(EntityTraits<SyncRowVersion>::tableName())
                         .where("deleted = 1 AND ts_ms < ?", {std::to_string(before_ts_ms)});
        return execDMLWithTxn(txn, query);
    }

private:
    static std::vector<SyncRowVersion> toRows(const toolkit::SqlitePool::SqlRetType &rows) {
        std::vector<SyncRowVersion> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<SyncRowVersion>::fromRow(row));
        }
        return ret;
    }
};

/**
 * Last-writer-wins bookkeeping used by SyncManager. Versions are compared by ts_ms, then by
 * peer_guid, so every node resolves concurrent writes the same way regardless of arrival order.
 */
class SyncRowVersionImp : public SyncRowVersionRepository {
public:
    using Ptr = std::shared_ptr<SyncRowVersionImp>;

    static std::string groupRowKey(const std::string &group_key) { return "#group:" + group_key; }

    // true if version a is strictly newer than version b.
    static bool isNewer(int64_t ts_a, const std::string &peer_a, int64_t ts_b, const std::string &peer_b) {
        return ts_a != ts_b ? ts_a > ts_b : peer_a > peer_b;
    }

    static bool isNewer(int64_t ts_ms, const std::string &peer_guid, const SyncRowVersion &v) {
        return isNewer(ts_ms, peer_guid, v.ts_ms, v.peer_guid);
    }

    bool findWithTxn(const std::string &table_name, const std::string &row_key, SyncRowVersion &out,
                     toolkit::SqliteTransaction::Ptr txn) {
        auto rows = findByKeyWithTxn(table_name, row_key, txn);
        if (rows.empty()) return false;
        out = rows[0];
        return true;
    }

    // Newest known version that governs `row_key`: its own version or, if the row belongs to a
    // group, the group's tombstone, whichever is newer.
    bool findEffectiveWithTxn(const std::string &table_name, const std::string &row_key, const std::string &group_key,
                              SyncRowVersion &out, toolkit::SqliteTransaction::Ptr txn) {
        bool found = !row_key.empty() && findWithTxn(table_name, row_key, out, txn);
        SyncRowVersion tomb;
        if (!group_key.empty() && findWithTxn(table_name, groupRowKey(group_key), tomb, txn)) {
            if (!found || isNewer(tomb.ts_ms, tomb.peer_guid, out)) {
                out = tomb;
                found = true;
            }
        }
        return found;
    }

    // Stores `v` unless an equal or newer version is already stored. Returns true if written.
    bool recordWithTxn(const SyncRowVersion &v, toolkit::SqliteTransaction::Ptr txn) {
        if (v.table_name.empty() || v.row_key.empty()) return false;
        SyncRowVersion current;
        if (findWithTxn(v.table_name, v.row_key, current, txn)) {
            if (!isNewer(v.ts_ms, v.peer_guid, current)) return false;
            return updateByIdWithTxn(v, txn);
        }
        return saveWithTxn(v, txn, true);
    }

    // row_keys of live group members written after version (ts_ms, peer_guid).
    std::set<std::string> liveKeysNewerThanWithTxn(const std::string &table_name, const std::string &group_key,
                                                    int64_t ts_ms, const std::string &peer_guid,
                                                    toolkit::SqliteTransaction::Ptr txn) {
        std::set<std::string> keys;
        for (const auto &v : findLiveInGroupNewerThanWithTxn(table_name, group_key, ts_ms, peer_guid, txn)) {
            keys.insert(v.row_key);
        }
        return keys;
    }

    // Stamps a local mutation: returns a timestamp >= `now_ms` that is newer than every stored
    // version governing `keys` (hybrid-clock style, so a lagging wall clock cannot make a local
    // write lose), and records the keys with that version. Runs in its own write transaction.
    int64_t stampLocal(std::vector<SyncRowVersion> keys, int64_t now_ms, const std::string &peer_guid) {
        auto txn = getExecutor()->beginWriteTxn();
        int64_t ts = now_ms;
        for (const auto &k : keys) {
            SyncRowVersion current;
            if (findEffectiveWithTxn(k.table_name, k.row_key, k.group_key, current, txn) &&
                !isNewer(ts, peer_guid, current)) {
                ts = current.ts_ms + 1;
            }
        }
        for (auto &k : keys) {
            k.ts_ms = ts;
            k.peer_guid = peer_guid;
            recordWithTxn(k, txn);
        }
        txn->commit();
        return ts;
    }

    // Tombstones older than the cut-off can no longer block anything useful: every peer has
    // long since received the delete (see Database::kRowVersionTombstoneTtlDays).
    void pruneTombstones(int64_t before_ts_ms) {
        auto txn = getExecutor()->beginWriteTxn();
        removeTombstonesBeforeWithTxn(before_ts_ms, txn);
        txn->commit();
    }
};

} // namespace managerkit

#endif // STORAGE_SYNC_ROW_VERSION_H
