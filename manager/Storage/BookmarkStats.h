#ifndef STORAGE_BOOKMARKSTATS_H
#define STORAGE_BOOKMARKSTATS_H

#include <string>
#include "DbStorage.h"

namespace managerkit {

struct BookmarkDeviceCount {
    std::string device_id;
    int count;
    int64_t size;
};

DECLARE_ENTITY(BookmarkDeviceCount, "bookmark_stats",
    {"device_id"},
    &BookmarkDeviceCount::device_id, "device_id", 
    &BookmarkDeviceCount::count, "count",
    &BookmarkDeviceCount::size, "size"
)

class BookmarkStatsRepository : public SqliteRepository<BookmarkDeviceCount> {
public:
    BookmarkStatsRepository() : SqliteRepository<BookmarkDeviceCount>(Database::kMediaServerDb) {}

    std::vector<BookmarkDeviceCount> findAll() {
        auto query = toolkit::QueryBuilder()
                            .select(EntityTraits<BookmarkDeviceCount>::getColumns())
                            .from(EntityTraits<BookmarkDeviceCount>::tableName());
        auto rows = _executor->executeRaw(query);
        std::vector<BookmarkDeviceCount> ret;
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<BookmarkDeviceCount>::fromRow(row));
        }
        return ret;
    }

protected:
    
};

class BookmarkStatsImp : public BookmarkStatsRepository {
public:
    using Ptr = std::shared_ptr<BookmarkStatsImp>;
    BookmarkStatsImp() : BookmarkStatsRepository() {}

    void add(const std::string &device_id, int amount) {
        auto statsCounts = findById(device_id);
        if (statsCounts.empty()) {
            if (amount > 0) {
                BookmarkDeviceCount statsCountInsert = { device_id, amount, 0 };
                save(statsCountInsert, true);
            } else {
                WarnL << "Invalid bookmark stats count increasing amount: " << amount;
            }
        } else {
            auto statsCount = statsCounts.begin();
            statsCount->count += amount;
            if (statsCount->count > 0) {
                updateById(*statsCount);
            } else {
                statsCount->count = 0;
                remove(device_id);
            }
        }
    }

    std::vector<BookmarkDeviceCount> findById(const std::string &device_id) {
        BookmarkDeviceCount bmStats_search;
        bmStats_search.device_id = device_id;
        return BookmarkStatsRepository::findById(bmStats_search);
    }

private:
    void remove(const std::string &device_id) {
        BookmarkDeviceCount statsCount = {device_id, 0 , 0};
        removeById(statsCount);
    }
};

} // namespace managerkit

#endif // STORAGE_BOOKMARKSTATS_H