#ifndef S3MANAGERKIT_BOOKMARK_H
#define S3MANAGERKIT_BOOKMARK_H

#include <string>
#include "DbStorage.h"
#include "BookmarkTag.h"
#include "BookmarkStats.h"
#include "BookmarkIndex.h"
#include "Util/util.h"
#include "Local/StatisticRecorder.h"
#include "Extension/Resource.h"

namespace managerkit {

struct Bookmark {
    std::string guid;
    std::string camera_guid;
    int64_t start_time;
    int duration;
    Optional<int64_t> end_time;
    Optional<std::string> name;
    Optional<std::string> description;
    Optional<int> timeout;
    Optional<std::string> creator_guid;
    Optional<int64_t> created;

    Json::Value toJson() const {
        Json::Value v;
        v["guid"] = guid;
        v["camera_guid"] = camera_guid;
        v["start_time"] = static_cast<Json::Int64>(start_time);
        v["duration"] = duration;
        if (end_time.has_value()) v["end_time"] = static_cast<Json::Int64>(end_time.value());
        if (name.has_value()) v["name"] = name.value();
        if (description.has_value()) v["description"] = description.value();
        if (timeout.has_value()) v["timeout"] = timeout.value();
        if (creator_guid.has_value()) v["creator_guid"] = creator_guid.value();
        if (created.has_value()) v["created"] = static_cast<Json::Int64>(created.value());
        return v;
    }

    static Bookmark fromJson(const Json::Value &v) {
        Bookmark b;
        b.guid = v["guid"].asString();
        b.camera_guid = v["camera_guid"].asString();
        b.start_time = v["start_time"].asInt64();
        b.duration = v["duration"].asInt();
        if (v.isMember("end_time")) b.end_time = Optional<int64_t>(v["end_time"].asInt64());
        if (v.isMember("name")) b.name = Optional<std::string>(v["name"].asString());
        if (v.isMember("description")) b.description = Optional<std::string>(v["description"].asString());
        if (v.isMember("timeout")) b.timeout = Optional<int>(v["timeout"].asInt());
        if (v.isMember("creator_guid")) b.creator_guid = Optional<std::string>(v["creator_guid"].asString());
        if (v.isMember("created")) b.created = Optional<int64_t>(v["created"].asInt64());
        return b;
    }
};

DECLARE_ENTITY(Bookmark, "bookmarks",
    {"guid"},
    &Bookmark::guid, "guid", 
    &Bookmark::camera_guid, "camera_guid", 
    &Bookmark::start_time, "start_time", 
    &Bookmark::duration, "duration",
    &Bookmark::end_time, "end_time",
    &Bookmark::name, "name",
    &Bookmark::description, "description",
    &Bookmark::timeout, "timeout",
    &Bookmark::creator_guid, "creator_guid",
    &Bookmark::created, "created"
)

class BookmarkRepository : public SqliteRepository<Bookmark> {
public:
    BookmarkRepository() : SqliteRepository<Bookmark>(Database::kMediaServerDb) {}

protected:
    std::vector<Bookmark> findByTimeRange(int64_t start_time, int64_t end_time, const std::vector<std::string> &camera_guids, const std::string &user_id,
                                        int offset, int limit, std::string &sort, const std::string& search_term = "") {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "start_time BETWEEN ? AND ?";
        whereParams.push_back(std::to_string(start_time));
        whereParams.push_back(std::to_string(end_time));

        if (camera_guids.size() > 0) {
            whereClause << " AND camera_guid IN (";
            for (size_t i = 0; i < camera_guids.size(); i++) {
                whereClause << " ? ";
                if (i + 1 < camera_guids.size()) whereClause << ",";
                whereParams.push_back(camera_guids[i]);
            }
            whereClause << ")";
        }

        if (!user_id.empty()){
            whereClause << " AND creator_guid = ?";
            whereParams.push_back(user_id);
        }

        if (!search_term.empty()){
            whereClause << " AND guid IN (SELECT guid FROM bookmark_fts WHERE bookmark_fts MATCH ? UNION SELECT bookmark_guid FROM bookmark_tag_fts WHERE bookmark_tag_fts MATCH ? ) ";
            whereParams.push_back(search_term);
            whereParams.push_back(search_term);
        }

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<Bookmark>::getColumns())
                         .from(EntityTraits<Bookmark>::tableName())
                         .where(whereClause.str(), whereParams)
                         .offset(offset)
                         .limit(limit)
                         .orderBy("start_time " + sort);
        auto rows = _executor->executeRaw(query);
        std::vector<Bookmark> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<Bookmark>::fromRow(row));
        }
        return ret;
    }

    int countByTimeRange(int64_t start_time, int64_t end_time, const std::vector<std::string> &camera_guids, const std::string &user_id, const std::string& search_term = "") {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "start_time BETWEEN ? AND ?";
        whereParams.push_back(std::to_string(start_time));
        whereParams.push_back(std::to_string(end_time));

        if (camera_guids.size() > 0) {
            whereClause << " AND camera_guid IN (";
            for (size_t i = 0; i < camera_guids.size(); i++) {
                whereClause << " ? ";
                if (i + 1 < camera_guids.size()) whereClause << ",";
                whereParams.push_back(camera_guids[i]);
            }
            whereClause << ")";
        }

        if (!user_id.empty()){
            whereClause << " AND creator_guid = ?";
            whereParams.push_back(user_id);
        }

        if (!search_term.empty()){
            whereClause << " AND guid IN (SELECT guid FROM bookmark_fts WHERE bookmark_fts MATCH ? UNION SELECT bookmark_guid FROM bookmark_tag_fts WHERE bookmark_tag_fts MATCH ? ) ";
            whereParams.push_back(search_term);
            whereParams.push_back(search_term);
        }

        auto query = toolkit::QueryBuilder()
                         .select({ "COUNT(*) AS total" })
                         .from(EntityTraits<Bookmark>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        return rows.empty() ? 0 : std::stoi(rows[0][0].c_str());
    }

    std::vector<Bookmark> findByIds(const std::vector<std::string> &guids) {
        if (guids.empty()) return {};
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;
        whereClause << "guid IN (";
        for (size_t i = 0; i < guids.size(); i++) {
            whereClause << "?";
            if (i + 1 < guids.size()) whereClause << ",";
            whereParams.push_back(guids[i]);
        }
        whereClause << ")";
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<Bookmark>::getColumns())
                         .from(EntityTraits<Bookmark>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<Bookmark> ret;
        ret.reserve(rows.size());
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<Bookmark>::fromRow(row));
        }
        return ret;
    }

    std::vector<Bookmark> findByTimeCreated(const std::vector<std::string> &camera_guids, const std::string &user_id, int limit, std::string &sort) {
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
                         .select(EntityTraits<Bookmark>::getColumns())
                         .from(EntityTraits<Bookmark>::tableName())
                         .where(whereClause.str(), whereParams)
                         .limit(limit)
                         .orderBy("created " + sort);
        auto rows = _executor->executeRaw(query);
        std::vector<Bookmark> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<Bookmark>::fromRow(row));
        }
        return ret;
    }

    std::vector<Bookmark> findByTimeRange(int64_t start_time, int64_t end_time, const std::string &camera_guid) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "start_time BETWEEN ? AND ?";
        whereParams.push_back(std::to_string(start_time));
        whereParams.push_back(std::to_string(end_time));

        if (!camera_guid.empty()) {
            whereClause << " AND camera_guid = ?";
            whereParams.push_back(camera_guid);
        }

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<Bookmark>::getColumns())
                         .from(EntityTraits<Bookmark>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        std::vector<Bookmark> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<Bookmark>::fromRow(row));
        }
        return ret;
    }
};

class BookmarkImp : public BookmarkRepository {
public:
    using Ptr = std::shared_ptr<BookmarkImp>;
    BookmarkImp() : BookmarkRepository() {
        _bTag = std::make_shared<BookmarkTagImp>();
        _bStats = std::make_shared<BookmarkStatsImp>();
        _bIdx  = std::make_shared<BookmarkIndexImp>();
    }

    void add(Bookmark &bm, const std::string &tags) { 
        if (bm.guid.empty()) {
            bm.guid = toolkit::generate_guid();
            DebugL << "Generate bookmark guid: " << bm.guid;
        }
        save(bm, true);
        _bTag->add(bm.guid, tags);
        _bStats->add(bm.camera_guid, 1);
        StatisticRecorder::Instance().addBookmarkCount(bm.camera_guid, bm.created ? bm.created.value() : 0, true);

        // Write summary to ESC DB so all cluster nodes can query this bookmark.
        BookmarkIndex idx;
        idx.bookmark_guid  = bm.guid;
        idx.camera_guid    = bm.camera_guid;
        idx.owner_peer_id  = ResourceManager::Instance().getSelfNodeId();
        idx.start_time     = bm.start_time;
        idx.end_time       = bm.end_time ? bm.end_time.value() : (bm.start_time + bm.duration * 1000LL);
        idx.name           = bm.name ? bm.name.value() : "";
        idx.description    = bm.description ? bm.description.value() : "";
        idx.tags_csv       = tags;          
        idx.creator_guid   = bm.creator_guid ? bm.creator_guid.value() : "";
        idx.created        = bm.created ? bm.created.value() : 0;
        _bIdx->add(idx);
    }

    void update(Bookmark &bm, const std::string &tags) { 
        updateById(bm);
        _bTag->add(bm.guid, tags, true);

        // Keep ESC DB index in sync with updated values.
        BookmarkIndex idx;
        idx.bookmark_guid  = bm.guid;
        idx.camera_guid    = bm.camera_guid;
        idx.owner_peer_id  = ResourceManager::Instance().getSelfNodeId();
        idx.start_time     = bm.start_time;
        idx.end_time       = bm.end_time ? bm.end_time.value() : (bm.start_time + bm.duration * 1000LL);
        idx.name           = bm.name ? bm.name.value() : "";
        idx.description    = bm.description ? bm.description.value() : "";
        idx.tags_csv       = tags;
        idx.creator_guid   = bm.creator_guid ? bm.creator_guid.value() : "";
        idx.created        = bm.created ? bm.created.value() : 0;
        _bIdx->add(idx);
    }

    void remove(const std::string &guid) {
        auto ret = findById(guid);
        if (ret.size()) {
            Bookmark bm = ret[0];
            removeById(bm);
            _bTag->remove(guid);
            _bStats->add(bm.camera_guid, -1);
            StatisticRecorder::Instance().addBookmarkCount(bm.camera_guid, bm.created ? bm.created.value() : 0, false);
            _bIdx->remove(guid);
        }
    }

    std::vector<Bookmark> findById(std::string id) {
        Bookmark bm_search;
        bm_search.guid = id;
        return BookmarkRepository::findById(bm_search);
    }

    // Fetch multiple bookmarks in a single query by a list of GUIDs.
    // Result order matches the DB row order (not the input order).
    std::vector<Bookmark> findByGuids(const std::vector<std::string> &guids) {
        return BookmarkRepository::findByIds(guids);
    }

    std::vector<Bookmark> search(int64_t start_time, int64_t end_time, const std::string &camera_guids, const std::string &user_id, 
                                const std::string &search, int page, int size, std::string sort) {
        std::vector<std::string> _camera_guids;
        if (!camera_guids.empty()) {
            _camera_guids = toolkit::split(camera_guids, ",");
        }
        std::string _sort = toolkit::strToLower(sort) == "desc" ? "DESC" : "ASC";
        int offset = page * size;
        toolkit::trim(const_cast<std::string&>(search));
        return findByTimeRange(start_time, end_time, _camera_guids, user_id, offset, size, _sort, search);
    }

    int count(int64_t start_time, int64_t end_time, const std::string &camera_guids, const std::string &user_id, const std::string &search) {
        std::vector<std::string> _camera_guids;
        if (!camera_guids.empty()) {
            _camera_guids = toolkit::split(camera_guids, ",");
        }
        return countByTimeRange(start_time, end_time, _camera_guids, user_id, search);
    }

    std::string findTagsByBookmark(const std::string &guid) { 
        auto ret = _bTag->findTagsByBookmark(guid);
        std::ostringstream oss;
        for (size_t i = 0; i < ret.size(); i++) {
            if (i > 0) {
                oss << ",";
            }
            oss << ret[i];
        }
        return oss.str();
    }

    std::vector<Bookmark> findRecentByCameraGuid(const std::string &camera_guids, const std::string &user_id, int size, std::string sort) {
        std::vector<std::string> _camera_guids;
        if (!camera_guids.empty()) {
            _camera_guids = toolkit::split(camera_guids, ",");
        }
        std::string _sort = toolkit::strToLower(sort) == "desc" ? "DESC" : "ASC";
        return findByTimeCreated(_camera_guids, user_id, size, _sort);
    }

    int removeByTimeRange(uint64_t start_time, uint64_t end_time, const std::string &camera_guid) {
        int removed_count = 0;
        auto ret = findByTimeRange(start_time, end_time, camera_guid);
        for (const auto& b : ret) {
            remove(b.guid);
            removed_count++;
        }
        return removed_count;
    }

private:
    BookmarkTagImp::Ptr   _bTag;
    BookmarkStatsImp::Ptr _bStats;
    BookmarkIndexImp::Ptr _bIdx;
};

} // namespace managerkit 

#endif // S3MANAGERKIT_BOOKMARK_H