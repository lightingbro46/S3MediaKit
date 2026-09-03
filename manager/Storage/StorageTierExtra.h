#ifndef STORAGE_TIER_EXTRA_H
#define STORAGE_TIER_EXTRA_H

#ifdef ENABLE_TIER_STORAGE

#include <ctime>
#include <sstream>
#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/util.h"

namespace managerkit {

struct RestoreJob {
    std::string job_id;
    std::string camera_id;
    std::string source_tier;
    std::string target_tier;
    std::string status;
    int64_t start_time = 0;
    int64_t end_time = 0;
    int64_t total_bytes = 0;
    int64_t processed_bytes = 0;
    Optional<std::string> reason;
    Optional<std::string> error_message;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

DECLARE_ENTITY(RestoreJob, "restore_jobs",
    {"job_id"},
    &RestoreJob::job_id,          "job_id",
    &RestoreJob::camera_id,       "camera_id",
    &RestoreJob::source_tier,     "source_tier",
    &RestoreJob::target_tier,     "target_tier",
    &RestoreJob::status,          "status",
    &RestoreJob::start_time,      "start_time",
    &RestoreJob::end_time,        "end_time",
    &RestoreJob::total_bytes,     "total_bytes",
    &RestoreJob::processed_bytes, "processed_bytes",
    &RestoreJob::reason,          "reason",
    &RestoreJob::error_message,   "error_message",
    &RestoreJob::created_at,      "created_at",
    &RestoreJob::updated_at,      "updated_at"
)

struct RestoreJobSegment {
    std::string job_id;
    std::string camera_id;
    std::string stream_id;
    std::string range_id;
    std::string segment_path;
    std::string source_pool_id;
    int64_t range_start = 0;
    int64_t range_end = 0;
    std::string status;
    int64_t total_bytes = 0;
    int64_t processed_bytes = 0;
    Optional<std::string> error_message;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

DECLARE_ENTITY(RestoreJobSegment, "restore_job_segments",
    MAKE_PK("job_id", "stream_id", "segment_path"),
    &RestoreJobSegment::job_id,          "job_id",
    &RestoreJobSegment::camera_id,       "camera_id",
    &RestoreJobSegment::stream_id,       "stream_id",
    &RestoreJobSegment::range_id,        "range_id",
    &RestoreJobSegment::segment_path,    "segment_path",
    &RestoreJobSegment::source_pool_id,  "source_pool_id",
    &RestoreJobSegment::range_start,     "range_start",
    &RestoreJobSegment::range_end,       "range_end",
    &RestoreJobSegment::status,          "status",
    &RestoreJobSegment::total_bytes,     "total_bytes",
    &RestoreJobSegment::processed_bytes, "processed_bytes",
    &RestoreJobSegment::error_message,   "error_message",
    &RestoreJobSegment::created_at,      "created_at",
    &RestoreJobSegment::updated_at,      "updated_at"
)

struct TierMetadataCleanupJob {
    std::string range_id;
    std::string camera_id;
    std::string status;
    int attempt_count = 0;
    Optional<std::string> error_message;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

DECLARE_ENTITY(TierMetadataCleanupJob, "tier_metadata_cleanup_jobs",
    {"range_id"},
    &TierMetadataCleanupJob::range_id,      "range_id",
    &TierMetadataCleanupJob::camera_id,     "camera_id",
    &TierMetadataCleanupJob::status,        "status",
    &TierMetadataCleanupJob::attempt_count, "attempt_count",
    &TierMetadataCleanupJob::error_message, "error_message",
    &TierMetadataCleanupJob::created_at,    "created_at",
    &TierMetadataCleanupJob::updated_at,    "updated_at"
)

struct SegmentRetentionOverride {
    std::string range_id;
    int64_t extend_until = 0;
    Optional<std::string> reason;
    int64_t updated_at = 0;
};

DECLARE_ENTITY(SegmentRetentionOverride, "segment_retention_overrides",
    {"range_id"},
    &SegmentRetentionOverride::range_id,     "range_id",
    &SegmentRetentionOverride::extend_until, "extend_until",
    &SegmentRetentionOverride::reason,       "reason",
    &SegmentRetentionOverride::updated_at,   "updated_at"
)

struct StorageAlert {
    std::string id;
    std::string level;
    std::string type;
    std::string pool_id;
    std::string message;
    int acknowledged = 0;
    int64_t created_at = 0;
};

DECLARE_ENTITY(StorageAlert, "storage_alerts",
    {"id"},
    &StorageAlert::id,           "id",
    &StorageAlert::level,        "level",
    &StorageAlert::type,         "type",
    &StorageAlert::pool_id,      "pool_id",
    &StorageAlert::message,      "message",
    &StorageAlert::acknowledged, "acknowledged",
    &StorageAlert::created_at,   "created_at"
)

struct ProtectedVideo {
    std::string protected_id;
    std::string camera_id;
    int64_t start_time = 0;
    int64_t end_time = 0;
    std::string type;
    Optional<std::string> reason;
    int64_t created_at = 0;
};

DECLARE_ENTITY(ProtectedVideo, "protected_videos",
    {"protected_id"},
    &ProtectedVideo::protected_id, "protected_id",
    &ProtectedVideo::camera_id,    "camera_id",
    &ProtectedVideo::start_time,   "start_time",
    &ProtectedVideo::end_time,     "end_time",
    &ProtectedVideo::type,         "type",
    &ProtectedVideo::reason,       "reason",
    &ProtectedVideo::created_at,   "created_at"
)

class RestoreJobRepository : public SqliteRepository<RestoreJob> {
public:
    RestoreJobRepository() : SqliteRepository<RestoreJob>(Database::kMediaServerDb) {}

    bool add(const RestoreJob &job) { return save(job, true); }

    std::vector<RestoreJob> findByJobId(const std::string &job_id) {
        RestoreJob j;
        j.job_id = job_id;
        return SqliteRepository<RestoreJob>::findById(j);
    }

    std::vector<RestoreJob> query(const std::string &camera_id = "",
                                  const std::string &status = "",
                                  int64_t from_time = 0,
                                  int64_t to_time = 0,
                                  int page = 0,
                                  int size = 20) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;
        addCond(where, params, has, "camera_id = ?", camera_id);
        addCond(where, params, has, "status = ?", status);
        if (from_time > 0) addCond(where, params, has, "created_at >= ?", std::to_string(from_time));
        if (to_time > 0) addCond(where, params, has, "created_at <= ?", std::to_string(to_time));

        auto q = toolkit::QueryBuilder()
            .select(EntityTraits<RestoreJob>::getColumns())
            .from(EntityTraits<RestoreJob>::tableName())
            .orderBy("created_at DESC")
            .limit(size)
            .offset(page * size);
        if (has) q = q.where(where.str(), params);

        auto rows = _executor->executeRaw(q);
        std::vector<RestoreJob> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<RestoreJob>::fromRow(row));
        return ret;
    }

    int countQuery(const std::string &camera_id = "",
                   const std::string &status = "",
                   int64_t from_time = 0,
                   int64_t to_time = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;
        addCond(where, params, has, "camera_id = ?", camera_id);
        addCond(where, params, has, "status = ?", status);
        if (from_time > 0) addCond(where, params, has, "created_at >= ?", std::to_string(from_time));
        if (to_time > 0) addCond(where, params, has, "created_at <= ?", std::to_string(to_time));

        auto q = toolkit::QueryBuilder()
            .select({"COUNT(*)"})
            .from(EntityTraits<RestoreJob>::tableName());
        if (has) q = q.where(where.str(), params);
        auto rows = _executor->executeRaw(q);
        return rows.empty() || rows[0].empty() ? 0 : std::stoi(rows[0][0]);
    }

    int countActive() {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"COUNT(*)"})
                .from(EntityTraits<RestoreJob>::tableName())
                .where("status = ? OR status = ?", {"PENDING", "RUNNING"}));
        return rows.empty() || rows[0].empty() ? 0 : std::stoi(rows[0][0]);
    }

    std::vector<RestoreJob> queryExpiredTerminal(int64_t cutoff_updated_at,
                                                 int size = 100) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<RestoreJob>::getColumns())
                .from(EntityTraits<RestoreJob>::tableName())
                .where("status IN (?, ?, ?) AND updated_at > 0 AND updated_at <= ?",
                       {"DONE", "FAILED", "CANCELLED", std::to_string(cutoff_updated_at)})
                .orderBy("updated_at ASC")
                .limit(size));

        std::vector<RestoreJob> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<RestoreJob>::fromRow(row));
        return ret;
    }

    std::vector<RestoreJob> queryOverlapping(const std::string &camera_id,
                                             int64_t start_time,
                                             int64_t end_time,
                                             int size = 20) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND start_time < ? AND end_time > ?";
        params.push_back(camera_id);
        params.push_back(std::to_string(end_time));
        params.push_back(std::to_string(start_time));

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<RestoreJob>::getColumns())
                .from(EntityTraits<RestoreJob>::tableName())
                .where(where.str(), params)
                .orderBy("updated_at DESC")
                .limit(size));

        std::vector<RestoreJob> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<RestoreJob>::fromRow(row));
        return ret;
    }

    bool updateStatusAt(const std::string &job_id,
                        const std::string &status,
                        int64_t updated_at,
                        int64_t processed_bytes = -1,
                        const std::string &error = "") {
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(updated_at)});
        if (processed_bytes >= 0)
            set.push_back({"processed_bytes", std::to_string(processed_bytes)});
        if (!error.empty())
            set.push_back({"error_message", error});

        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<RestoreJob>::tableName())
                .set(set)
                .where("job_id = ?", {job_id}));
    }

    bool addWithSegments(const RestoreJob &job,
                         const std::vector<RestoreJobSegment> &segments) {
        auto txn = _executor->execTxn();
        try {
            if (!saveWithTxn(job, txn, true))
                return false;
            for (const auto &segment : segments) {
                const auto columns = EntityTraits<RestoreJobSegment>::getColumns();
                const auto values = EntityTraits<RestoreJobSegment>::getValues(segment);
                std::vector<std::pair<std::string, std::string>> assignments;
                for (size_t i = 0; i < columns.size(); ++i)
                    assignments.push_back(std::make_pair(columns[i], values[i]));
                auto query = toolkit::QueryBuilder()
                    .insertInto(EntityTraits<RestoreJobSegment>::tableName())
                    .values(assignments);
                if (!_executor->execDMLWithTxn(txn, query))
                    return false;
            }
            txn->commit();
            return true;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }

private:
    static void addCond(std::ostringstream &where,
                        std::vector<std::string> &params,
                        bool &has,
                        const std::string &clause,
                        const std::string &value) {
        if (value.empty()) return;
        if (has) where << " AND ";
        where << clause;
        params.push_back(value);
        has = true;
    }
};

class RestoreJobSegmentRepository : public SqliteRepository<RestoreJobSegment> {
public:
    RestoreJobSegmentRepository() : SqliteRepository<RestoreJobSegment>(Database::kMediaServerDb) {}

    std::vector<RestoreJobSegment> queryByJob(const std::string &job_id) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<RestoreJobSegment>::getColumns())
                .from(EntityTraits<RestoreJobSegment>::tableName())
                .where("job_id = ?", {job_id})
                .orderBy("range_start ASC"));
        std::vector<RestoreJobSegment> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<RestoreJobSegment>::fromRow(row));
        return ret;
    }

    std::vector<RestoreJobSegment> queryActiveSegment(const std::string &camera_id,
                                                      const std::string &stream_id,
                                                      const std::string &segment_path) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<RestoreJobSegment>::getColumns())
                .from(EntityTraits<RestoreJobSegment>::tableName())
                .where("camera_id = ? AND stream_id = ? AND segment_path = ? "
                       "AND status IN (?, ?)",
                       {camera_id, stream_id, segment_path, "PENDING", "RUNNING"})
                .limit(1));
        std::vector<RestoreJobSegment> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<RestoreJobSegment>::fromRow(row));
        return ret;
    }

    std::vector<RestoreJobSegment> claimPendingAt(int limit, int64_t updated_at) {
        std::vector<RestoreJobSegment> claimed;
        auto txn = _executor->execTxn();
        try {
            auto rows = _executor->executeRawWithTxn(
                txn,
                toolkit::QueryBuilder()
                    .select(EntityTraits<RestoreJobSegment>::getColumns())
                    .from(EntityTraits<RestoreJobSegment>::tableName())
                    .where("status = ?", {"PENDING"})
                    .orderBy("created_at ASC")
                    .limit(limit));
            for (const auto &row : rows) {
                RestoreJobSegment item = EntityTraits<RestoreJobSegment>::fromRow(row);
                bool ok = _executor->execDMLWithTxn(
                    txn,
                    toolkit::QueryBuilder()
                        .update(EntityTraits<RestoreJobSegment>::tableName())
                        .set({{"status", "RUNNING"}, {"updated_at", std::to_string(updated_at)}})
                        .where("job_id = ? AND stream_id = ? AND segment_path = ? AND status = ?",
                               {item.job_id, item.stream_id, item.segment_path, "PENDING"}));
                if (ok) {
                    item.status = "RUNNING";
                    item.updated_at = updated_at;
                    claimed.push_back(item);
                }
            }
            txn->commit();
        } catch (...) {
            txn->rollback();
            throw;
        }
        return claimed;
    }

    bool updateStatusAt(const RestoreJobSegment &segment,
                        const std::string &status,
                        int64_t processed_bytes,
                        int64_t updated_at,
                        const std::string &error = "") {
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"processed_bytes", std::to_string(processed_bytes)});
        set.push_back({"updated_at", std::to_string(updated_at)});
        set.push_back({"error_message", error});
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<RestoreJobSegment>::tableName())
                .set(set)
                .where("job_id = ? AND stream_id = ? AND segment_path = ?",
                       {segment.job_id, segment.stream_id, segment.segment_path}));
    }

    void recoverRunningAt(int64_t updated_at) {
        _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<RestoreJobSegment>::tableName())
                .set({{"status", "PENDING"}, {"updated_at", std::to_string(updated_at)}})
                .where("status = ?", {"RUNNING"}));
    }
};

class TierMetadataCleanupJobRepository : public SqliteRepository<TierMetadataCleanupJob> {
public:
    TierMetadataCleanupJobRepository() : SqliteRepository<TierMetadataCleanupJob>(Database::kMediaServerDb) {}

    bool add(const TierMetadataCleanupJob &job) { return save(job, true); }

    bool markRangeDeletedAndInsert(const TierMetadataCleanupJob &job,
                                   const std::string &final_status) {
        auto txn = _executor->execTxn();
        if (!_executor->execDMLWithTxn(
                txn,
                toolkit::QueryBuilder()
                    .update("segment_tier_ranges")
                    .set({{"status", final_status}, {"updated_at", std::to_string(job.updated_at)}})
                    .where("range_id = ?", {job.range_id})))
            return false;
        if (!saveWithTxn(job, txn, true))
            return false;
        txn->commit();
        return true;
    }

    std::vector<TierMetadataCleanupJob> queryPending(int limit = 100) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<TierMetadataCleanupJob>::getColumns())
                .from(EntityTraits<TierMetadataCleanupJob>::tableName())
                .where("status = ?", {"PENDING"})
                .orderBy("updated_at ASC")
                .limit(limit));
        std::vector<TierMetadataCleanupJob> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<TierMetadataCleanupJob>::fromRow(row));
        return ret;
    }

    bool updateResultAt(const std::string &range_id,
                        const std::string &status,
                        int64_t updated_at,
                        const std::string &error = "") {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"attempt_count"})
                .from(EntityTraits<TierMetadataCleanupJob>::tableName())
                .where("range_id = ?", {range_id})
                .limit(1));
        int attempts = rows.empty() || rows[0].empty() ? 1 : std::stoi(rows[0][0]) + 1;
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<TierMetadataCleanupJob>::tableName())
                .set({{"status", status},
                      {"attempt_count", std::to_string(attempts)},
                      {"error_message", error},
                      {"updated_at", std::to_string(updated_at)}})
                .where("range_id = ?", {range_id}));
    }
};

class SegmentRetentionOverrideRepository : public SqliteRepository<SegmentRetentionOverride> {
public:
    SegmentRetentionOverrideRepository() : SqliteRepository<SegmentRetentionOverride>(Database::kMediaServerDb) {}

    bool saveOrUpdate(const SegmentRetentionOverride &item) {
        auto existing = findByRange(item.range_id);
        return existing.empty() ? save(item, true) : updateById(item);
    }

    bool persistAndReactivateRange(const SegmentRetentionOverride &item) {
        auto txn = _executor->execTxn();
        auto existing = _executor->executeRawWithTxn(
            txn,
            toolkit::QueryBuilder()
                .select({"1"})
                .from(EntityTraits<SegmentRetentionOverride>::tableName())
                .where("range_id = ?", {item.range_id})
                .limit(1));
        bool ok = false;
        if (existing.empty()) {
            ok = saveWithTxn(item, txn, true);
        } else {
            ok = updateByIdWithTxn(item, txn);
        }
        if (!ok)
            return false;
        if (!_executor->execDMLWithTxn(
                txn,
                toolkit::QueryBuilder()
                    .update("segment_tier_ranges")
                    .set({{"status", "AVAILABLE"}, {"updated_at", std::to_string(item.updated_at)}})
                    .where("range_id = ?", {item.range_id})))
            return false;
        txn->commit();
        return true;
    }

    std::vector<SegmentRetentionOverride> findByRange(const std::string &range_id) {
        SegmentRetentionOverride item;
        item.range_id = range_id;
        return SqliteRepository<SegmentRetentionOverride>::findById(item);
    }

    bool isActive(const std::string &range_id, int64_t now) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"1"})
                .from(EntityTraits<SegmentRetentionOverride>::tableName())
                .where("range_id = ? AND extend_until > ?", {range_id, std::to_string(now)})
                .limit(1));
        return !rows.empty();
    }
};

class StorageAlertRepository : public SqliteRepository<StorageAlert> {
public:
    StorageAlertRepository() : SqliteRepository<StorageAlert>(Database::kMediaServerDb) {}

    bool add(const StorageAlert &alert) { return save(alert, true); }

    std::vector<StorageAlert> query(const std::string &level = "",
                                    int acknowledged_filter = -1,
                                    int page = 0,
                                    int size = 20) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;
        if (!level.empty()) {
            where << "level = ?";
            params.push_back(level);
            has = true;
        }
        if (acknowledged_filter >= 0) {
            if (has) where << " AND ";
            where << "acknowledged = ?";
            params.push_back(std::to_string(acknowledged_filter));
            has = true;
        }
        auto q = toolkit::QueryBuilder()
            .select(EntityTraits<StorageAlert>::getColumns())
            .from(EntityTraits<StorageAlert>::tableName())
            .orderBy("created_at DESC")
            .limit(size)
            .offset(page * size);
        if (has) q = q.where(where.str(), params);
        auto rows = _executor->executeRaw(q);
        std::vector<StorageAlert> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<StorageAlert>::fromRow(row));
        return ret;
    }

    int countQuery(const std::string &level = "", int acknowledged_filter = -1) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;
        if (!level.empty()) {
            where << "level = ?";
            params.push_back(level);
            has = true;
        }
        if (acknowledged_filter >= 0) {
            if (has) where << " AND ";
            where << "acknowledged = ?";
            params.push_back(std::to_string(acknowledged_filter));
            has = true;
        }
        auto q = toolkit::QueryBuilder()
            .select({"COUNT(*)"})
            .from(EntityTraits<StorageAlert>::tableName());
        if (has) q = q.where(where.str(), params);
        auto rows = _executor->executeRaw(q);
        return rows.empty() || rows[0].empty() ? 0 : std::stoi(rows[0][0]);
    }

    bool acknowledge(const std::string &id) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<StorageAlert>::tableName())
                .set({{"acknowledged", "1"}})
                .where("id = ?", {id}));
    }
};

class ProtectedVideoRepository : public SqliteRepository<ProtectedVideo> {
public:
    ProtectedVideoRepository() : SqliteRepository<ProtectedVideo>(Database::kMediaServerDb) {}

    bool add(const ProtectedVideo &video) { return save(video, true); }

    bool remove(const std::string &protected_id) {
        ProtectedVideo v;
        v.protected_id = protected_id;
        return SqliteRepository<ProtectedVideo>::removeById(v);
    }

    std::vector<ProtectedVideo> query(const std::string &camera_id = "",
                                      const std::string &type = "",
                                      int64_t from_time = 0,
                                      int64_t to_time = 0,
                                      int page = 0,
                                      int size = 20) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;
        if (!camera_id.empty()) addCond(where, params, has, "camera_id = ?", camera_id);
        if (!type.empty()) addCond(where, params, has, "type = ?", type);
        if (from_time > 0) addCond(where, params, has, "end_time >= ?", std::to_string(from_time));
        if (to_time > 0) addCond(where, params, has, "start_time <= ?", std::to_string(to_time));

        auto q = toolkit::QueryBuilder()
            .select(EntityTraits<ProtectedVideo>::getColumns())
            .from(EntityTraits<ProtectedVideo>::tableName())
            .orderBy("created_at DESC")
            .limit(size)
            .offset(page * size);
        if (has) q = q.where(where.str(), params);
        auto rows = _executor->executeRaw(q);
        std::vector<ProtectedVideo> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<ProtectedVideo>::fromRow(row));
        return ret;
    }

    int countQuery(const std::string &camera_id = "",
                   const std::string &type = "",
                   int64_t from_time = 0,
                   int64_t to_time = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;
        if (!camera_id.empty()) addCond(where, params, has, "camera_id = ?", camera_id);
        if (!type.empty()) addCond(where, params, has, "type = ?", type);
        if (from_time > 0) addCond(where, params, has, "end_time >= ?", std::to_string(from_time));
        if (to_time > 0) addCond(where, params, has, "start_time <= ?", std::to_string(to_time));

        auto q = toolkit::QueryBuilder()
            .select({"COUNT(*)"})
            .from(EntityTraits<ProtectedVideo>::tableName());
        if (has) q = q.where(where.str(), params);
        auto rows = _executor->executeRaw(q);
        return rows.empty() || rows[0].empty() ? 0 : std::stoi(rows[0][0]);
    }

    bool overlaps(const std::string &camera_id, int64_t start_time, int64_t end_time) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"COUNT(*)"})
                .from(EntityTraits<ProtectedVideo>::tableName())
                .where("camera_id = ? AND start_time <= ? AND end_time >= ?",
                       {camera_id, std::to_string(end_time), std::to_string(start_time)}));
        return !rows.empty() && !rows[0].empty() && std::stoi(rows[0][0]) > 0;
    }

private:
    static void addCond(std::ostringstream &where,
                        std::vector<std::string> &params,
                        bool &has,
                        const std::string &clause,
                        const std::string &value) {
        if (value.empty()) return;
        if (has) where << " AND ";
        where << clause;
        params.push_back(value);
        has = true;
    }
};

class RestoreJobImp : public RestoreJobRepository {
public:
    using Ptr = std::shared_ptr<RestoreJobImp>;

    bool updateStatus(const std::string &job_id,
                      const std::string &status,
                      int64_t processed_bytes = -1,
                      const std::string &error = "") {
        return updateStatusAt(job_id, status, static_cast<int64_t>(time(nullptr)),
                              processed_bytes, error);
    }
};

class RestoreJobSegmentImp : public RestoreJobSegmentRepository {
public:
    using Ptr = std::shared_ptr<RestoreJobSegmentImp>;

    std::vector<RestoreJobSegment> claimPending(int limit) {
        return claimPendingAt(limit, static_cast<int64_t>(time(nullptr)));
    }

    bool updateStatus(const RestoreJobSegment &segment,
                      const std::string &status,
                      int64_t processed_bytes,
                      const std::string &error = "") {
        return updateStatusAt(segment, status, processed_bytes,
                              static_cast<int64_t>(time(nullptr)), error);
    }

    void recoverRunning() {
        recoverRunningAt(static_cast<int64_t>(time(nullptr)));
    }
};

class TierMetadataCleanupJobImp : public TierMetadataCleanupJobRepository {
public:
    using Ptr = std::shared_ptr<TierMetadataCleanupJobImp>;

    bool markRangeDeletedAndEnqueue(const std::string &range_id,
                                    const std::string &camera_id,
                                    const std::string &final_status) {
        if (range_id.empty() || camera_id.empty())
            return false;
        const int64_t now = static_cast<int64_t>(time(nullptr));
        TierMetadataCleanupJob job;
        job.range_id = range_id;
        job.camera_id = camera_id;
        job.status = "PENDING";
        job.created_at = now;
        job.updated_at = now;
        return markRangeDeletedAndInsert(job, final_status);
    }

    bool updateResult(const std::string &range_id,
                      bool done,
                      const std::string &error = "") {
        return updateResultAt(range_id, done ? "DONE" : "PENDING",
                              static_cast<int64_t>(time(nullptr)), error);
    }
};

class SegmentRetentionOverrideImp : public SegmentRetentionOverrideRepository {
public:
    using Ptr = std::shared_ptr<SegmentRetentionOverrideImp>;

    bool upsert(const SegmentRetentionOverride &item) {
        if (item.range_id.empty() || item.extend_until <= 0)
            return false;
        return saveOrUpdate(item);
    }

    bool extendRange(const SegmentRetentionOverride &item) {
        if (item.range_id.empty() || item.extend_until <= 0)
            return false;
        return persistAndReactivateRange(item);
    }
};

class StorageAlertImp : public StorageAlertRepository {
public:
    using Ptr = std::shared_ptr<StorageAlertImp>;
};

class ProtectedVideoImp : public ProtectedVideoRepository {
public:
    using Ptr = std::shared_ptr<ProtectedVideoImp>;
};

} // namespace managerkit

#endif // STORAGE_TIER_EXTRA_H

#endif // ENABLE_TIER_STORAGE
