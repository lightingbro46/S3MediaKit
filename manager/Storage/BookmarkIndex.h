#ifndef STORAGE_BOOKMARK_INDEX_H
#define STORAGE_BOOKMARK_INDEX_H

#include <string>
#include <json/json.h>
#include "DbStorage.h"
#include "TransactionLog.h"
#include "Util/util.h"
#include "Extension/TableSyncHandler.h"

namespace managerkit {

/**
 * BookmarkIndex is a lightweight summary of a bookmark stored in the ESC DB (cluster-synced).
 * It enables time-range queries across the entire cluster without fanning out to all nodes.
 * Full bookmark detail (description, tags, FTS index) remains in the per-node kMediaServerDb.
 */
struct BookmarkIndex {
    std::string bookmark_guid;
    std::string camera_guid;
    std::string owner_peer_id; // node that holds the full detail
    int64_t     start_time  = 0;
    int64_t     end_time    = 0;
    std::string name;
    std::string description;
    std::string tags_csv;      // comma-separated tags
    std::string creator_guid;
    int64_t     created     = 0;

    Json::Value toJson() const {
        Json::Value v;
        v["bookmark_guid"]  = bookmark_guid;
        v["camera_guid"]    = camera_guid;
        v["owner_peer_id"]  = owner_peer_id;
        v["start_time"]     = static_cast<Json::Int64>(start_time);
        v["end_time"]       = static_cast<Json::Int64>(end_time);
        v["name"]           = name;
        v["description"]    = description;
        v["tags_csv"]       = tags_csv;
        v["creator_guid"]   = creator_guid;
        v["created"]        = static_cast<Json::Int64>(created);
        return v;
    }

    static BookmarkIndex fromJson(const Json::Value &v) {
        BookmarkIndex idx;
        idx.bookmark_guid  = v["bookmark_guid"].asString();
        idx.camera_guid    = v["camera_guid"].asString();
        idx.owner_peer_id  = v["owner_peer_id"].asString();
        idx.start_time     = v["start_time"].asInt64();
        idx.end_time       = v["end_time"].asInt64();
        idx.name           = v["name"].asString();
        idx.description    = v["description"].asString();
        idx.tags_csv       = v["tags_csv"].asString();
        idx.creator_guid   = v["creator_guid"].asString();
        idx.created        = v["created"].asInt64();
        return idx;
    }
};

DECLARE_ENTITY(BookmarkIndex, "bookmark_index",
    {"bookmark_guid"},
    &BookmarkIndex::bookmark_guid, "bookmark_guid",
    &BookmarkIndex::camera_guid,   "camera_guid",
    &BookmarkIndex::owner_peer_id, "owner_peer_id",
    &BookmarkIndex::start_time,    "start_time",
    &BookmarkIndex::end_time,      "end_time",
    &BookmarkIndex::name,          "name",
    &BookmarkIndex::creator_guid,  "creator_guid",
    &BookmarkIndex::created,       "created"
)

class BookmarkIndexRepository : public SqliteRepository<BookmarkIndex> {
public:
    BookmarkIndexRepository() : SqliteRepository<BookmarkIndex>(Database::kEdgeStorageControllerDb) {}

protected:
    std::vector<BookmarkIndex> findByTimeRange(
            int64_t start_time, int64_t end_time,
            const std::string &camera_guid, const std::string &creator_guid,
            const std::string &search_term,
            int offset, int limit, const std::string &sort) {
        std::ostringstream where;
        std::vector<std::string> params;

        where << "start_time BETWEEN ? AND ?";
        params.push_back(std::to_string(start_time));
        params.push_back(std::to_string(end_time));

        if (!camera_guid.empty()) {
            where << " AND camera_guid = ?";
            params.push_back(camera_guid);
        }
        if (!creator_guid.empty()) {
            where << " AND creator_guid = ?";
            params.push_back(creator_guid);
        }
        std::ostringstream join;
        if (!search_term.empty()) {
            join << "bookmark_index_fts ON bookmark_index_fts.rowid = bookmark_index.rowid";
            where << " AND bookmark_index_fts MATCH ? ";
            params.push_back(search_term);
        }

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<BookmarkIndex>::getColumns())
                         .from(EntityTraits<BookmarkIndex>::tableName());
        if (!join.str().empty()) {
            query.join(join.str());
        }
        query.where(where.str(), params)
             .orderBy("start_time " + sort)
             .offset(offset)
             .limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<BookmarkIndex> ret;
        for (const auto &r : rows)
            ret.push_back(EntityTraits<BookmarkIndex>::fromRow(r));
        return ret;
    }

    int countByTimeRange(int64_t start_time, int64_t end_time,
                         const std::string &camera_guid, const std::string &creator_guid,
                         const std::string &search_term = "") {
        std::ostringstream where;
        std::vector<std::string> params;

        where << "start_time BETWEEN ? AND ?";
        params.push_back(std::to_string(start_time));
        params.push_back(std::to_string(end_time));

        if (!camera_guid.empty()) {
            where << " AND camera_guid = ?";
            params.push_back(camera_guid);
        }
        if (!creator_guid.empty()) {
            where << " AND creator_guid = ?";
            params.push_back(creator_guid);
        }
        std::ostringstream join;
        if (!search_term.empty()) {
            join << "bookmark_index_fts ON bookmark_index_fts.rowid = bookmark_index.rowid";
            where << " AND bookmark_index_fts MATCH ? ";
            params.push_back(search_term);
        }

        auto query = toolkit::QueryBuilder()
                         .select({"COUNT(*) AS total"})
                         .from(EntityTraits<BookmarkIndex>::tableName());
        if (!join.str().empty()) {
            query.join(join.str());
        }
        query.where(where.str(), params);
        auto rows = _executor->executeRaw(query);
        return rows.empty() ? 0 : std::stoi(rows[0][0].c_str());
    }

    bool removeByGuid(const std::string &guid) {
        auto query = toolkit::QueryBuilder()
                         .deleteFrom(EntityTraits<BookmarkIndex>::tableName())
                         .where("bookmark_guid = ?", {guid});
        return _executor->execDML(query) > 0;
    }

    std::vector<BookmarkIndex> findByTimeCreated(const std::vector<std::string> &camera_guids, const std::string &user_id, int limit, std::string &sort) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        if (camera_guids.size() > 0) {
            whereClause << " camera_guid IN (";
            for (size_t i = 0; i < camera_guids.size(); i++) {
                whereClause << " ? ";
                if (i + 1 < camera_guids.size()) whereClause << ",";
                whereParams.push_back(camera_guids[i]);
            }
            whereClause << ")";
        }

        if (!user_id.empty()) {
            if (!whereClause.str().empty()) {
                whereClause << " AND ";
            }
            whereClause << " creator_guid = ?";
            whereParams.push_back(user_id);
        }

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<BookmarkIndex>::getColumns())
                         .from(EntityTraits<BookmarkIndex>::tableName())
                         .where(whereClause.str(), whereParams)
                         .limit(limit)
                         .orderBy("created " + sort);
        auto rows = _executor->executeRaw(query);
        std::vector<BookmarkIndex> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<BookmarkIndex>::fromRow(row));
        }
        return ret;
    }
};

class BookmarkIndexImp : public BookmarkIndexRepository {
public:
    using Ptr = std::shared_ptr<BookmarkIndexImp>;

    BookmarkIndexImp() : BookmarkIndexRepository() {
        _log_impl = std::make_shared<TransactionLogImp>();
    }

    // Upsert: insert or update by bookmark_guid PK.
    void add(BookmarkIndex &entry, bool append_log = true) {
        auto existing = findById(entry);
        if (!existing.empty()) {
            updateById(entry);
        } else {
            save(entry, true); // include_id=true to include bookmark_guid in INSERT
        }
        if (append_log) {
            _log_impl->appendLocalDataMutation(EntityTraits<BookmarkIndex>::tableName(), TRAN_DATA_OP_UPSERT, entry.toJson());
        }
    }

    void remove(const std::string &guid, bool append_log = true) {
        removeByGuid(guid);
        if (append_log) {
            Json::Value payload;
            payload["bookmark_guid"] = guid;
            _log_impl->appendLocalDataMutation(EntityTraits<BookmarkIndex>::tableName(), TRAN_DATA_OP_DELETE, payload);
        }
    }

    // Look up a single bookmark_index entry by its primary key.
    std::vector<BookmarkIndex> findByBookmarkGuid(const std::string &guid) {
        BookmarkIndex key;
        key.bookmark_guid = guid;
        return findById(key);
    }

    // Paginated search. page is 0-based (consistent with BookmarkImp::search).
    std::vector<BookmarkIndex> search(int64_t start_time, int64_t end_time,
                                      const std::string &camera_guid, const std::string &creator_guid,
                                      const std::string &search_term,
                                      int page, int size, const std::string &sort) {
        std::string sort_copy = sort;
        std::string _sort = toolkit::strToLower(sort_copy) == "desc" ? "DESC" : "ASC";
        int offset = page * size;
        return findByTimeRange(start_time, end_time, camera_guid, creator_guid, search_term, offset, size, _sort);
    }

    int count(int64_t start_time, int64_t end_time, const std::string &camera_guid,
              const std::string &creator_guid, const std::string &search_term = "") {
        return countByTimeRange(start_time, end_time, camera_guid, creator_guid, search_term);
    }

    std::vector<BookmarkIndex> findRecentByCameraGuid(const std::string &camera_guids, const std::string &user_id, int size, std::string sort) {
        std::vector<std::string> _camera_guids;
        if (!camera_guids.empty()) {
            _camera_guids = toolkit::split(camera_guids, ",");
        }
        std::string _sort = toolkit::strToLower(sort) == "desc" ? "DESC" : "ASC";
        return findByTimeCreated(_camera_guids, user_id, size, _sort);
    }

private:
    TransactionLogImp::Ptr _log_impl;

public:
    static TableSyncHandler makeSyncHandler() {
        TableSyncHandler h;
        h.rowKey = [](const Json::Value &p) -> std::string {
            return p["bookmark_guid"].asString();
        };
        h.onUpsert = [](const Json::Value &p) {
            auto imp = std::make_shared<BookmarkIndexImp>();
            auto idx = BookmarkIndex::fromJson(p);
            imp->add(idx, false);
        };
        h.onUpsertBatch = nullptr;
        h.onDelete = [](const Json::Value &p) {
            auto imp = std::make_shared<BookmarkIndexImp>();
            std::string guid = p["bookmark_guid"].asString();
            if (!guid.empty()) imp->remove(guid, false);
        };
        h.onSnapshot = [](const Json::Value &arr) {
            auto imp = std::make_shared<BookmarkIndexImp>();
            for (const auto &v : arr) {
                auto idx = BookmarkIndex::fromJson(v);
                imp->add(idx, false);
            }
        };
        return h;
    }
};

} // namespace managerkit

#endif // STORAGE_BOOKMARK_INDEX_H
