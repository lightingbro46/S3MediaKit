#ifndef STORAGE_RECORDENCRYPTIONTIERRANGE_H
#define STORAGE_RECORDENCRYPTIONTIERRANGE_H

#include <string>
#include <vector>
#include <set>
#include <sstream>
#include "DbStorage.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

struct RecordEncryptionTierRange {
    std::string tier_id;
    int64_t start_time = 0;
    int64_t end_time = 0;
    std::string salt_enc;
    std::string salt_hmac;
    std::string key_hash;
    int mode = 0;
    int64_t created_at = 0;
};

static std::vector<std::string> time_range_key = { "tier_id", "start_time" };
DECLARE_ENTITY(RecordEncryptionTierRange, "record_encryption_tier_ranges",
    { time_range_key },
    &RecordEncryptionTierRange::tier_id, "tier_id",
    &RecordEncryptionTierRange::start_time, "start_time",
    &RecordEncryptionTierRange::end_time, "end_time",
    &RecordEncryptionTierRange::salt_enc, "salt_enc",
    &RecordEncryptionTierRange::salt_hmac, "salt_hmac",
    &RecordEncryptionTierRange::key_hash, "key_hash",
    &RecordEncryptionTierRange::mode, "mode",
    &RecordEncryptionTierRange::created_at, "created_at"
)

class RecordEncryptionTierRangeRepository : public SqliteRepository<RecordEncryptionTierRange> {
public:
    RecordEncryptionTierRangeRepository() 
        : SqliteRepository<RecordEncryptionTierRange>(Database::kMediaServerDb) {}

    std::vector<RecordEncryptionTierRange> findByTierId(const std::string& tier_id) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "tier_id = ?";
        whereParams.push_back(tier_id);

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<RecordEncryptionTierRange>::getColumns())
                         .from(EntityTraits<RecordEncryptionTierRange>::tableName())
                         .where(whereClause.str(), whereParams)
                         .orderBy("start_time ASC");

        auto rows = _executor->executeRaw(query);

        std::vector<RecordEncryptionTierRange> ret;
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<RecordEncryptionTierRange>::fromRow(row));
        }
        return ret;
    }

    bool deleteByTierIdAndStart(const std::string& tier_id, int64_t start_time) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "tier_id = ? AND start_time = ?";
        whereParams.push_back(tier_id);
        whereParams.push_back(std::to_string(start_time));

        auto query = toolkit::QueryBuilder()
                         .deleteFrom(EntityTraits<RecordEncryptionTierRange>::tableName())
                         .where(whereClause.str(), whereParams);

        return _executor->execDML(query) > 0;
    }

    bool deleteByTierId(const std::string& tier_id) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "tier_id = ?";
        whereParams.push_back(tier_id);

        auto query = toolkit::QueryBuilder()
                         .deleteFrom(EntityTraits<RecordEncryptionTierRange>::tableName())
                         .where(whereClause.str(), whereParams);

        return _executor->execDML(query) > 0;
    }

    bool insertOrReplace(const RecordEncryptionTierRange& range) {
        return save(range, true);
    }

    bool insertBatch(const std::vector<RecordEncryptionTierRange>& ranges) {
        if (ranges.empty()) {
            return true;
        }

        for (const auto& range : ranges) {
            if (!insertOrReplace(range)) {
                return false;
            }
        }
        return true;
    }

    bool closeOpenTimeRange(const std::string& tier_id, int64_t end_time) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "tier_id = ? AND end_time = ?";
        whereParams.push_back(tier_id);
        whereParams.push_back(std::to_string(INT64_MAX));

        auto query = toolkit::QueryBuilder()
                         .update(EntityTraits<RecordEncryptionTierRange>::tableName())
                         .set({{"end_time", std::to_string(end_time)}})
                         .where(whereClause.str(), whereParams);

        return _executor->execDML(query) > 0;
    }

    std::vector<RecordEncryptionTierRange> findOpenTimeRange(const std::string& tier_id) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "tier_id = ? AND end_time = ?";
        whereParams.push_back(tier_id);
        whereParams.push_back(std::to_string(INT64_MAX));

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<RecordEncryptionTierRange>::getColumns())
                         .from(EntityTraits<RecordEncryptionTierRange>::tableName())
                         .where(whereClause.str(), whereParams);

        auto rows = _executor->executeRaw(query);

        std::vector<RecordEncryptionTierRange> ret;
        for (const auto& row : rows) {
            ret.push_back(EntityTraits<RecordEncryptionTierRange>::fromRow(row));
        }
        return ret;
    }
};

class RecordEncryptionTierRangeImp : public RecordEncryptionTierRangeRepository {
public:
    using Ptr = std::shared_ptr<RecordEncryptionTierRangeImp>;
    RecordEncryptionTierRangeImp() : RecordEncryptionTierRangeRepository() {}

    bool saveTimeRanges(const std::string& tier_id, 
                        const std::vector<RecordEncryptionTierRange>& ranges) {
        if (ranges.empty()) {
            // Never erase the whole history. A caller that wants the tier gone
            // should use deleteByTierId() explicitly.
            return false;
        }

        if (!insertBatch(ranges)) {
            return false;
        }

        std::set<int64_t> keep;
        for (const auto& r : ranges) {
            keep.insert(r.start_time);
        }
        for (const auto& existing : findByTierId(tier_id)) {
            if (keep.count(existing.start_time) == 0) {
                deleteByTierIdAndStart(tier_id, existing.start_time);
            }
        }
        return true;
    }

    std::vector<RecordEncryptionTierRange> loadTimeRanges(const std::string& tier_id) {
        return findByTierId(tier_id);
    }

    bool addTimeRange(RecordEncryptionTierRange range) {
        range.created_at = (int64_t)time(nullptr);
        return insertOrReplace(range);
    }

    bool closeOpenRange(const std::string& tier_id, int64_t end_time) {
        return closeOpenTimeRange(tier_id, end_time);
    }
};

} // namespace managerkit

#endif // STORAGE_RECORDENCRYPTIONTIERRANGE_H
