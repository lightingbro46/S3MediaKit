#ifndef STORAGE_BOOKMARK_INDEX_H
#define STORAGE_BOOKMARK_INDEX_H

#include <string>
#include <json/json.h>
#include "DbStorage.h"
#include "TransactionLog.h"
#include "Util/util.h"

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
        if (!search_term.empty()) {
            where << " JOIN bookmark_index_fts ON bookmark_index_fts.bookmark_guid = bookmark_index.bookmark_guid WHERE bookmark_index_fts MATCH ? ";
            params.push_back(search_term);
        }

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<BookmarkIndex>::getColumns())
                         .from(EntityTraits<BookmarkIndex>::tableName())
                         .where(where.str(), params)
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
        if (!search_term.empty()) {
            where << " JOIN bookmark_index_fts ON bookmark_index_fts.bookmark_guid = bookmark_index.bookmark_guid WHERE bookmark_index_fts MATCH ? ";
            params.push_back(search_term);
        }

        auto query = toolkit::QueryBuilder()
                         .select({"COUNT(*) AS total"})
                         .from(EntityTraits<BookmarkIndex>::tableName())
                         .where(where.str(), params);
        auto rows = _executor->executeRaw(query);
        return rows.empty() ? 0 : std::stoi(rows[0][0].c_str());
    }

    bool removeByGuid(const std::string &guid) {
        auto query = toolkit::QueryBuilder()
                         .deleteFrom(EntityTraits<BookmarkIndex>::tableName())
                         .where("bookmark_guid = ?", {guid});
        return _executor->execDML(query) > 0;
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

private:
    TransactionLogImp::Ptr _log_impl;
};

} // namespace managerkit

#endif // STORAGE_BOOKMARK_INDEX_H
