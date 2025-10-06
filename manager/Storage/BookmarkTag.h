#ifndef STORAGE_BOOKMARKTAG_H
#define STORAGE_BOOKMARKTAG_H

#include <string>
#include "DbStorage.h"
#include "BookmarkTagCount.h"

namespace managerkit {

struct BookmarkTag {
    std::string bookmark_guid;
    std::string name;
};

DECLARE_ENTITY(BookmarkTag, "bookmark_tags",
    {"name"},
    &BookmarkTag::bookmark_guid, "bookmark_guid", 
    &BookmarkTag::name, "name"
)

class BookmarkTagRepository : public SqliteRepository<BookmarkTag> {
public:
    BookmarkTagRepository() : SqliteRepository<BookmarkTag>(Database::kMediaServerDb) {}

protected:
    std::vector<BookmarkTag> findByBookmark(const std::string &bmGuid) {
        auto query = toolkit::QueryBuilder()
                             .select(EntityTraits<BookmarkTag>::getColumns())
                             .from(EntityTraits<BookmarkTag>::tableName())
                             .where("bookmark_guid= ?", { bmGuid });
        auto rows = _executor->executeRaw(query);
        std::vector<BookmarkTag> ret;
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<BookmarkTag>::fromRow(row));
        }
        return ret;
    }

    bool removeByBookmarkGuid(const std::string &bmGuid) {
        auto query = toolkit::QueryBuilder()
                             .deleteFrom(EntityTraits<BookmarkTag>::tableName())
                             .where("bookmark_guid= ?", { bmGuid });
        return _executor->execDML(query) > 0;
    }
};

class BookmarkTagImp : public BookmarkTagRepository {
public:
    using Ptr = std::shared_ptr<BookmarkTagImp>;
    BookmarkTagImp() : BookmarkTagRepository() {
        _bTagCount = std::make_shared<BookmarkTagCountImp>();
    }

    void add(const std::string &bmGuid, const std::string &tagstring, bool update = false) {
        if (!update && tagstring.empty())
            return;

        // remove previous tags
        remove(bmGuid);

        // add new tags
        if (tagstring.empty()) {
            return;
        }
        auto tags = toolkit::split(tagstring, ",");
        for (const auto &tag : tags) {
            BookmarkTag bTag = { bmGuid, tag };
            save(bTag, true);
            _bTagCount->add(tag, 1);
        }
    }

    void remove(const std::string &bmGuid) {
        auto tagsAdded = findTagsByBookmark(bmGuid);
        removeByBookmarkGuid(bmGuid);
        for (const auto &tag : tagsAdded) {
            _bTagCount->add(tag, -1);
        }
    }

    std::vector<std::string> findTagsByBookmark(const std::string &bmGuid) {
        auto bmTags = findByBookmark(bmGuid);
        std::vector<std::string> tags;
        for (const auto &item : bmTags) {
            tags.push_back(item.name);
        }
        return tags;
    }

private:
    BookmarkTagCountImp::Ptr _bTagCount;
};

} // namespace managerkit

#endif // STORAGE_BOOKMARKTAG_H