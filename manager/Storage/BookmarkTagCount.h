#ifndef STORAGE_BOOKMARKTAGCOUNT_H
#define STORAGE_BOOKMARKTAGCOUNT_H

#include <string>
#include "DbStorage.h"

namespace managerkit {

struct BookmargTagCount {
    std::string tag;
    int count;
};

DECLARE_ENTITY(BookmargTagCount, "bookmark_tag_counts",
    {"tag"},
    &BookmargTagCount::tag, "tag", 
    &BookmargTagCount::count, "count"
)

class BookmarkTagCountRepository : public SqliteRepository<BookmargTagCount> {
public:
    BookmarkTagCountRepository(const std::string &tag) : SqliteRepository<BookmargTagCount>(tag) {}

protected:
    std::vector<BookmargTagCount> findByCountDesc(int limit = 1) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<BookmargTagCount>::getColumns())
                         .from(EntityTraits<BookmargTagCount>::tableName())
                         .orderBy("count DESC")
                         .limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<BookmargTagCount> ret;
        for (const auto& row : rows) {
           ret.push_back(EntityTraits<BookmargTagCount>::fromRow(row));
        }
        return ret;
    }
};

class BookmarkTagCountImp : public BookmarkTagCountRepository {
public:    
    using Ptr = std::shared_ptr<BookmarkTagCountImp>;
    BookmarkTagCountImp() : BookmarkTagCountRepository(Database::kMediaServerDb) {}
    
    void add(const std::string &tag, int amount) {
        BookmargTagCount tagCountFind = { tag, 0 };
        auto tagCounts = findById(tagCountFind);
        if (tagCounts.empty()) {
            BookmargTagCount tagCountInsert = { tag, 1 };
            save(tagCountInsert, true);
        } else {
            auto tagCount = tagCounts.begin();
            tagCount->count += amount;
            if (tagCount->count < 0) {
                tagCount->count = 0;
            }
            updateById(*tagCount);
        }
    }

    std::vector<std::string> findTagsByCountDesc(int limit = 1) {
        auto tagCounts = findByCountDesc(limit);
        std::vector<std::string> ret;
        for (const auto& entity : tagCounts) {
            ret.push_back(entity.tag);
        }
        return ret;
    }
};

} // namespace managerkit


#endif // STORAGE_BOOKMARKTAGCOUNT_H