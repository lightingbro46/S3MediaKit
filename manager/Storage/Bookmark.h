#ifndef S3MANAGERKIT_BOOKMARK_H
#define S3MANAGERKIT_BOOKMARK_H

#include <string>
#include "DbStorage.h"
#include "BookmarkTag.h"
#include "Util/util.h"

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
    std::vector<Bookmark> findByTimeRange(int64_t start_time, int64_t end_time, const std::vector<std::string> &camera_guids,
                                        int offset, int limit, std::string &sort) {
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

    int countByTimeRange(int64_t start_time, int64_t end_time, const std::vector<std::string> &camera_guids) {
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

        auto query = toolkit::QueryBuilder()
                         .select({ "COUNT(*) AS total" })
                         .from(EntityTraits<Bookmark>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        return rows.empty() ? 0 : std::stoi(rows[0][0].c_str());
    }
};

class BookmarkImp : public BookmarkRepository {
public:
    using Ptr = std::shared_ptr<BookmarkImp>;
    BookmarkImp() : BookmarkRepository() {
        _bTag = std::make_shared<BookmarkTagImp>();
    }

    void add(Bookmark &bm, const std::string &tags) { 
        if (bm.guid.empty()) {
            bm.guid = toolkit::generate_guid();
            DebugL << "Generate bookmark guid: " << bm.guid;
        }
        save(bm, true);
        _bTag->add(bm.guid, tags);
    }

    void update(Bookmark &bm, const std::string &tags) { 
        updateById(bm);
        _bTag->add(bm.guid, tags, true);
    }

    void remove(const std::string &guid) { 
        Bookmark bm;
        bm.guid = guid;
        removeById(bm);
        _bTag->remove(guid);
    }

    std::vector<Bookmark> findById(std::string id) {
        Bookmark bm_search;
        bm_search.guid = id;
        return BookmarkRepository::findById(bm_search);
    }

    std::vector<Bookmark> search(int64_t start_time, int64_t end_time, const std::string &camera_guids, const std::string &search,
                                int page, int size, std::string sort) {
        std::vector<std::string> _camera_guids;
        if (!camera_guids.empty()) {
            _camera_guids = toolkit::split(camera_guids, ",");
        }
        std::string _sort = toolkit::strToLower(sort) == "desc" ? "DESC" : "ASC";
        int offset = (page - 1) * size;
        return findByTimeRange(start_time, end_time, _camera_guids, offset, size, _sort);
    }

    int count(int64_t start_time, int64_t end_time, const std::string &camera_guids, const std::string &search) {
        std::vector<std::string> _camera_guids;
        if (!camera_guids.empty()) {
            _camera_guids = toolkit::split(camera_guids, ",");
        }
        return countByTimeRange(start_time, end_time, _camera_guids);
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

private:
    BookmarkTagImp::Ptr _bTag;
};

} // namespace managerkit 

#endif // S3MANAGERKIT_BOOKMARK_H