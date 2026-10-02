#ifndef STORAGE_LOCAL_RESOURCE_STATUS_GAP_H
#define STORAGE_LOCAL_RESOURCE_STATUS_GAP_H

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Common/StrUtil.h"

namespace managerkit {

/**
 * @brief Represents one offline interval of a local resource.
 *
 * A row is created when the resource disconnects and is closed by setting
 * reconnect_time when it reconnects. No ONLINE rows are stored.
 */
struct LocalResourceStatusGap {
    std::string resource_id;
    std::string gap_id;
    uint64_t disconnect_time = 0; 
    uint64_t reconnect_time = 0;
    std::string disconnect_reason;
    int state = 1; // ResourceStatus::OFFLINE
    uint64_t updated_time = 0;
};

DECLARE_ENTITY(LocalResourceStatusGap, "local_resource_status_gap",
    MAKE_PK("resource_id", "gap_id"),
    &LocalResourceStatusGap::resource_id, "resource_id",
    &LocalResourceStatusGap::gap_id, "gap_id",
    &LocalResourceStatusGap::disconnect_time, "disconnect_time",
    &LocalResourceStatusGap::reconnect_time, "reconnect_time",
    &LocalResourceStatusGap::disconnect_reason, "disconnect_reason",
    &LocalResourceStatusGap::state, "state",
    &LocalResourceStatusGap::updated_time, "updated_time"
)

class LocalResourceStatusGapRepository : public SqliteRepository<LocalResourceStatusGap> {
public:
    LocalResourceStatusGapRepository() : SqliteRepository<LocalResourceStatusGap>(Database::kMediaServerDb) {}

    std::vector<LocalResourceStatusGap> findByResourceIdAndTimeRange(const std::string &resource_id, uint64_t start_time = 0, uint64_t end_time = UINT64_MAX) {
        if (resource_id.empty()) {
            return {};
        }
        return findByResourceIdsAndTimeRange({resource_id}, start_time, end_time);
    }

    std::vector<LocalResourceStatusGap> findByResourceIdsAndTimeRange(const std::vector<std::string> &resource_ids,
                                                                      uint64_t start_time = 0,
                                                                      uint64_t end_time = UINT64_MAX) {
        if (resource_ids.empty()) {
            return {};
        }
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "resource_id IN (";
        for (size_t i = 0; i < resource_ids.size(); ++i) {
            if (i) whereClause << ',';
            whereClause << '?';
            whereParams.push_back(resource_ids[i]);
        }
        whereClause << ") AND state = 1";
        if (end_time < UINT64_MAX) {
            whereClause << " AND disconnect_time < ?";
            whereParams.push_back(std::to_string(end_time));
        }
        if (start_time > 0) {
            // Half-open overlap test: [disconnect_time, reconnect_time) intersects
            // [start_time, end_time). reconnect_time=0 denotes an open gap.
            whereClause << " AND (reconnect_time = 0 OR reconnect_time > ?)";
            whereParams.push_back(std::to_string(start_time));
        }

        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<LocalResourceStatusGap>::getColumns())
                        .from(EntityTraits<LocalResourceStatusGap>::tableName())
                        .where(whereClause.str(), whereParams)
                        .orderBy("resource_id ASC, disconnect_time ASC");
        auto rows = _executor->executeRaw(query);
        std::vector<LocalResourceStatusGap> result;
        result.reserve(rows.size());
        for (const auto &row : rows) {
            result.emplace_back(EntityTraits<LocalResourceStatusGap>::fromRow(row));
        }
        return result;
    }

    bool findOpenGap(const std::string &resource_id, LocalResourceStatusGap &gap) {
        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<LocalResourceStatusGap>::getColumns())
                        .from(EntityTraits<LocalResourceStatusGap>::tableName())
                        .where("resource_id = ? AND state = 1 AND reconnect_time = 0", {resource_id})
                        .orderBy("disconnect_time DESC")
                        .limit(1);
        auto rows = _executor->executeRaw(query);
        if (rows.empty()) {
            return false;
        }
        gap = EntityTraits<LocalResourceStatusGap>::fromRow(rows.front());
        return true;
    }

    bool findOpenGapWithTxn(const std::string &resource_id, LocalResourceStatusGap &gap,
                            const toolkit::SqliteTransaction::Ptr &txn) {
        auto query = toolkit::QueryBuilder()
                        .select(EntityTraits<LocalResourceStatusGap>::getColumns())
                        .from(EntityTraits<LocalResourceStatusGap>::tableName())
                        .where("resource_id = ? AND state = 1 AND reconnect_time = 0", {resource_id})
                        .orderBy("disconnect_time DESC")
                        .limit(1);
        auto rows = executeRawWithTxn(txn, query);
        if (rows.empty()) {
            return false;
        }
        gap = EntityTraits<LocalResourceStatusGap>::fromRow(rows.front());
        return true;
    }

    bool removeByResourceIdAndThreshold(const std::string &resource_id, uint64_t threshold_time) {
        auto query = toolkit::QueryBuilder()
                        .deleteFrom(EntityTraits<LocalResourceStatusGap>::tableName())
                        .where("resource_id = ? AND reconnect_time != 0 AND reconnect_time < ?", {resource_id, std::to_string(threshold_time)});
        return _executor->execDML(query) > 0;
    }

    bool removeClosedBefore(uint64_t threshold_time) {
        auto query = toolkit::QueryBuilder()
                        .deleteFrom(EntityTraits<LocalResourceStatusGap>::tableName())
                        .where("reconnect_time != 0 AND reconnect_time < ?", {std::to_string(threshold_time)});
        return _executor->execDML(query) > 0;
    }

    std::vector<LocalResourceStatusGap> findClosedBefore(uint64_t threshold_time,
                                                         size_t limit = 100) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<LocalResourceStatusGap>::getColumns())
                         .from(EntityTraits<LocalResourceStatusGap>::tableName())
                         .where("reconnect_time != 0 AND reconnect_time < ?",
                                {std::to_string(threshold_time)})
                         .orderBy("reconnect_time ASC")
                         .limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<LocalResourceStatusGap> result;
        for (const auto &row : rows)
            result.emplace_back(EntityTraits<LocalResourceStatusGap>::fromRow(row));
        return result;
    }

    std::vector<LocalResourceStatusGap> findClosedBeforeForResource(const std::string &resource_id,
                                                                    uint64_t threshold_time,
                                                                    size_t limit = 100) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<LocalResourceStatusGap>::getColumns())
                         .from(EntityTraits<LocalResourceStatusGap>::tableName())
                         .where("resource_id = ? AND reconnect_time != 0 AND reconnect_time < ?",
                                {resource_id, std::to_string(threshold_time)})
                         .orderBy("reconnect_time ASC")
                         .limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<LocalResourceStatusGap> result;
        for (const auto &row : rows)
            result.emplace_back(EntityTraits<LocalResourceStatusGap>::fromRow(row));
        return result;
    }

    std::vector<std::string> findResourceIdsClosedBefore(uint64_t threshold_time) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"DISTINCT resource_id"})
                .from(EntityTraits<LocalResourceStatusGap>::tableName())
                .where("reconnect_time != 0 AND reconnect_time < ?",
                       {std::to_string(threshold_time)}));
        std::vector<std::string> ids;
        for (const auto &row : rows)
            if (!row.empty()) ids.push_back(row[0]);
        return ids;
    }
};

class LocalResourceStatusGapImp : public LocalResourceStatusGapRepository {
public:
    LocalResourceStatusGapImp() : LocalResourceStatusGapRepository() {}

    bool addOrUpdate(const LocalResourceStatusGap &gap) {
        if (gap.gap_id.empty() || gap.resource_id.empty()) {
            return false;
        }
        if (!findById(gap).empty()) {
            return updateById(gap);
        }
        return save(gap, true);
    }

    bool addWithTxn(const LocalResourceStatusGap &gap, const toolkit::SqliteTransaction::Ptr &txn) {
        return !gap.gap_id.empty() && !gap.resource_id.empty() && saveWithTxn(gap, txn, true);
    }

    bool updateWithTxn(const LocalResourceStatusGap &gap, const toolkit::SqliteTransaction::Ptr &txn) {
        return !gap.gap_id.empty() && !gap.resource_id.empty() && updateByIdWithTxn(gap, txn);
    }

    bool removeWithTxn(const LocalResourceStatusGap &gap,
                       const toolkit::SqliteTransaction::Ptr &txn) {
        return !gap.gap_id.empty() && !gap.resource_id.empty() &&
               removeByIdWithTxn(gap, txn);
    }

    bool removeByThresholdTime(const std::string &resource_id, uint64_t threshold_time) {
        if (resource_id.empty() || threshold_time == 0) {
            return false;
        }
        
        return removeByResourceIdAndThreshold(resource_id, threshold_time);
    }
};

} // namespace managerkit

#endif // STORAGE_LOCAL_RESOURCE_STATUS_GAP_H
