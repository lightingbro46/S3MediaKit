#ifndef STORAGE_TIERINGJOB_H
#define STORAGE_TIERINGJOB_H

#ifdef ENABLE_TIER_STORAGE

#include <algorithm>
#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/util.h"
#include "Common/StrUtil.h"
#include "Local/StorageTier.h"
#include "StorageTierExtra.h"

namespace managerkit {

// ===================================================================
// TieringJob entity — one row per data-movement job
// ===================================================================
struct TieringJob {
    std::string job_id;
    std::string camera_id;
    std::string stream_id;
    std::string range_id;
    std::string source_tier;       // HOT | WARM | COLD
    std::string target_tier;
    std::string source_pool_id;
    std::string target_pool_id;
    std::string status;            // PENDING | RUNNING | DONE | FAILED | CANCELLED
    int64_t segment_start_time = 0;
    int64_t segment_end_time   = 0;
    int64_t bytes_total        = 0;
    int64_t bytes_moved        = 0;
    Optional<std::string> error_message;
    int64_t created_at         = 0;
    int64_t updated_at         = 0;
    std::string job_trigger    = tieringJobTriggerToString(TieringJobTrigger::LEGACY);
    std::string policy_id;
    int64_t policy_updated_at  = 0;

    Json::Value toJson() const {
        Json::Value v;
        v["job_id"]              = job_id;
        v["camera_id"]           = camera_id;
        v["stream_id"]           = stream_id;
        v["range_id"]            = range_id;
        v["source_tier"]         = source_tier;
        v["target_tier"]         = target_tier;
        v["source_pool_id"]      = source_pool_id;
        v["target_pool_id"]      = target_pool_id;
        v["status"]              = status;
        v["segment_start_time"]  = static_cast<Json::Int64>(segment_start_time);
        v["segment_end_time"]    = static_cast<Json::Int64>(segment_end_time);
        v["bytes_total"]         = static_cast<Json::Int64>(bytes_total);
        v["bytes_moved"]         = static_cast<Json::Int64>(bytes_moved);
        v["error_message"]       = error_message.value_or("");
        v["created_at"]          = static_cast<Json::Int64>(created_at);
        v["updated_at"]          = static_cast<Json::Int64>(updated_at);
        v["job_trigger"]         = job_trigger;
        v["policy_id"]           = policy_id;
        v["policy_updated_at"]   = static_cast<Json::Int64>(policy_updated_at);
        return v;
    }
};

DECLARE_ENTITY(TieringJob, "tiering_jobs",
    {"job_id"},
    &TieringJob::job_id,              "job_id",
    &TieringJob::camera_id,           "camera_id",
    &TieringJob::stream_id,           "stream_id",
    &TieringJob::range_id,            "range_id",
    &TieringJob::source_tier,         "source_tier",
    &TieringJob::target_tier,         "target_tier",
    &TieringJob::source_pool_id,      "source_pool_id",
    &TieringJob::target_pool_id,      "target_pool_id",
    &TieringJob::status,              "status",
    &TieringJob::segment_start_time,  "segment_start_time",
    &TieringJob::segment_end_time,    "segment_end_time",
    &TieringJob::bytes_total,         "bytes_total",
    &TieringJob::bytes_moved,         "bytes_moved",
    &TieringJob::error_message,       "error_message",
    &TieringJob::created_at,          "created_at",
    &TieringJob::updated_at,          "updated_at",
    &TieringJob::job_trigger,         "job_trigger",
    &TieringJob::policy_id,           "policy_id",
    &TieringJob::policy_updated_at,   "policy_updated_at"
)

// ===================================================================
// SegmentTierRange entity — compact tier tracking over a time window
// ===================================================================
struct SegmentTierRange {
    std::string range_id;
    std::string camera_id;
    std::string stream_id;
    std::string tier;           // HOT | WARM | COLD
    std::string pool_id;
    std::string status;         // AVAILABLE | RESTORING | EXPIRED | DELETED | MISSING
    int64_t start_time    = 0;
    int64_t end_time      = 0;
    int64_t segment_count = 0;
    int64_t size_bytes    = 0;
    int64_t created_at    = 0;
    int64_t updated_at    = 0;

    Json::Value toJson() const {
        Json::Value v;
        v["range_id"]      = range_id;
        v["camera_id"]     = camera_id;
        v["stream_id"]     = stream_id;
        v["tier"]          = tier;
        v["pool_id"]       = pool_id;
        v["status"]        = status;
        v["start_time"]    = static_cast<Json::Int64>(start_time);
        v["end_time"]      = static_cast<Json::Int64>(end_time);
        v["segment_count"] = static_cast<Json::Int64>(segment_count);
        v["size_bytes"]    = static_cast<Json::Int64>(size_bytes);
        return v;
    }
};

DECLARE_ENTITY(SegmentTierRange, "segment_tier_ranges",
    {"range_id"},
    &SegmentTierRange::range_id,      "range_id",
    &SegmentTierRange::camera_id,     "camera_id",
    &SegmentTierRange::stream_id,     "stream_id",
    &SegmentTierRange::tier,          "tier",
    &SegmentTierRange::pool_id,       "pool_id",
    &SegmentTierRange::status,        "status",
    &SegmentTierRange::start_time,    "start_time",
    &SegmentTierRange::end_time,      "end_time",
    &SegmentTierRange::segment_count, "segment_count",
    &SegmentTierRange::size_bytes,    "size_bytes",
    &SegmentTierRange::created_at,    "created_at",
    &SegmentTierRange::updated_at,    "updated_at"
)

// ===================================================================
// PoolMetrics entity — time-series health snapshot
// ===================================================================
struct PoolMetrics {
    std::string pool_id;
    int64_t     timestamp   = 0;
    int64_t     used_bytes  = 0;
    int64_t     total_bytes = 0;
    float       usage_pct   = 0.0f;
    std::string health_status;   // OK | WARNING | HIGH | CRITICAL | OFFLINE
};

DECLARE_ENTITY(PoolMetrics, "pool_metrics",
    MAKE_PK("pool_id", "timestamp"),
    &PoolMetrics::pool_id,       "pool_id",
    &PoolMetrics::timestamp,     "timestamp",
    &PoolMetrics::used_bytes,    "used_bytes",
    &PoolMetrics::total_bytes,   "total_bytes",
    &PoolMetrics::usage_pct,     "usage_pct",
    &PoolMetrics::health_status, "health_status"
)

// ===================================================================
// Repository — TieringJob
// ===================================================================
class TieringJobRepository : public SqliteRepository<TieringJob> {
public:
    TieringJobRepository() : SqliteRepository<TieringJob>(Database::kMediaServerDb) {}

    std::vector<TieringJob> query(const std::string &camera_id   = "",
                                  const std::string &status      = "",
                                  const std::string &source_tier = "",
                                  const std::string &target_tier = "",
                                  int64_t from_time = 0, int64_t to_time = 0,
                                  int page = 0, int size = 20) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;

        auto cond = [&](const std::string &clause, const std::string &val) {
            if (!val.empty()) {
                if (has) where << " AND ";
                where << clause;
                params.push_back(val);
                has = true;
            }
        };

        cond("camera_id = ?",   camera_id);
        cond("status = ?",      status);
        cond("source_tier = ?", source_tier);
        cond("target_tier = ?", target_tier);
        if (from_time > 0) { if (has) where << " AND "; where << "created_at >= ?"; params.push_back(std::to_string(from_time)); has = true; }
        if (to_time   > 0) { if (has) where << " AND "; where << "created_at <= ?"; params.push_back(std::to_string(to_time));   has = true; }

        auto q = toolkit::QueryBuilder()
            .select(EntityTraits<TieringJob>::getColumns())
            .from(EntityTraits<TieringJob>::tableName())
            .orderBy("created_at DESC")
            .limit(size)
            .offset(page * size);
        if (has) q = q.where(where.str(), params);

        auto rows = _executor->executeRaw(q);
        std::vector<TieringJob> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<TieringJob>::fromRow(row));
        return ret;
    }

    int countQuery(const std::string &camera_id   = "",
                   const std::string &status      = "",
                   int64_t from_time = 0, int64_t to_time = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        bool has = false;
        if (!camera_id.empty()) { where << "camera_id = ?"; params.push_back(camera_id); has = true; }
        if (!status.empty())    { if (has) where << " AND "; where << "status = ?"; params.push_back(status); has = true; }
        if (from_time > 0)      { if (has) where << " AND "; where << "created_at >= ?"; params.push_back(std::to_string(from_time)); has = true; }
        if (to_time   > 0)      { if (has) where << " AND "; where << "created_at <= ?"; params.push_back(std::to_string(to_time));   has = true; }

        auto q = toolkit::QueryBuilder()
            .select({"COUNT(*)"})
            .from(EntityTraits<TieringJob>::tableName());
        if (has) q = q.where(where.str(), params);

        auto rows = _executor->executeRaw(q);
        if (!rows.empty() && !rows[0].empty())
            return std::stoi(rows[0][0]);
        return 0;
    }

    std::vector<TieringJob> findByStatus(const std::string &status, int limit = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "status = ?";
        params.push_back(status);

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<TieringJob>::getColumns())
                .from(EntityTraits<TieringJob>::tableName())
                .where(where.str(), params)
                .orderBy("created_at ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<TieringJob> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<TieringJob>::fromRow(row));
        return ret;
    }

    std::vector<TieringJob> findByJobId(const std::string &job_id) {
        TieringJob j;
        j.job_id = job_id;
        return SqliteRepository<TieringJob>::findById(j);
    }

    bool hasActiveJobForRange(const std::string &range_id) {
        if (range_id.empty())
            return false;
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"1"})
                .from(EntityTraits<TieringJob>::tableName())
                .where("range_id = ? AND status IN (?, ?)",
                       {range_id,
                        jobStatusToString(JobStatus::PENDING),
                        jobStatusToString(JobStatus::RUNNING)})
                .limit(1));
        return !rows.empty();
    }

    // Legacy jobs may not carry a range_id, so they are matched by window.
    bool hasActiveJobForWindow(const std::string &camera_id,
                               const std::string &stream_id,
                               const std::string &source_tier,
                               const std::string &target_tier,
                               int64_t start_time,
                               int64_t end_time) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"1"})
                .from(EntityTraits<TieringJob>::tableName())
                .where("camera_id = ? AND stream_id = ? AND source_tier = ? AND "
                       "target_tier = ? AND segment_start_time = ? AND "
                       "segment_end_time = ? AND status IN (?, ?)",
                       {camera_id, stream_id, source_tier, target_tier,
                        std::to_string(start_time), std::to_string(end_time),
                        jobStatusToString(JobStatus::PENDING),
                        jobStatusToString(JobStatus::RUNNING)})
                .limit(1));
        return !rows.empty();
    }

    // Bytes still expected to leave a pool through queued or running transfers.
    int64_t sumActiveBytesBySourcePool(const std::string &pool_id) {
        if (pool_id.empty())
            return 0;
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"COALESCE(SUM(bytes_total), 0)"})
                .from(EntityTraits<TieringJob>::tableName())
                .where("source_pool_id = ? AND status IN (?, ?)",
                       {pool_id,
                        jobStatusToString(JobStatus::PENDING),
                        jobStatusToString(JobStatus::RUNNING)}));
        if (rows.empty() || rows[0].empty() || rows[0][0].empty())
            return 0;
        return std::stoll(rows[0][0]);
    }

    // Newest first. error_prefix is compared literally, not as a LIKE pattern.
    std::vector<TieringJob> findRecentFailedByRange(const std::string &range_id,
                                                    const std::string &error_prefix,
                                                    int limit) {
        std::vector<TieringJob> ret;
        if (range_id.empty() || error_prefix.empty() || limit <= 0)
            return ret;
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<TieringJob>::getColumns())
                .from(EntityTraits<TieringJob>::tableName())
                .where("range_id = ? AND status = ? AND "
                       "substr(error_message, 1, ?) = ?",
                       {range_id, jobStatusToString(JobStatus::FAILED),
                        std::to_string(error_prefix.size()), error_prefix})
                .orderBy("updated_at DESC")
                .limit(limit));
        for (const auto &row : rows)
            ret.push_back(EntityTraits<TieringJob>::fromRow(row));
        return ret;
    }

    bool updateStatusAt(const std::string &job_id, const std::string &status,
                        int64_t updated_at,
                        int64_t bytes_moved = -1, const std::string &error = "") {
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(updated_at)});
        if (bytes_moved >= 0) set.push_back({"bytes_moved", std::to_string(bytes_moved)});
        if (!error.empty())   set.push_back({"error_message", error});

        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<TieringJob>::tableName())
            .set(set)
            .where("job_id = ?", {job_id});
        return _executor->execDML(q);
    }

    bool transitionStatusAt(const std::string &job_id,
                            const std::string &from_status,
                            const std::string &to_status,
                            int64_t updated_at,
                            int64_t bytes_moved = -1,
                            const std::string &error = "") {
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", to_status});
        set.push_back({"updated_at", std::to_string(updated_at)});
        if (bytes_moved >= 0) set.push_back({"bytes_moved", std::to_string(bytes_moved)});
        set.push_back({"error_message", error});
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<TieringJob>::tableName())
                .set(set)
                .where("job_id = ? AND status = ?", {job_id, from_status}));
    }

    std::vector<TieringJob> claimPendingAt(int limit, int64_t updated_at) {
        auto txn = _executor->execTxn();
        std::vector<TieringJob> claimed;
        auto rows = _executor->executeRawWithTxn(
            txn,
            toolkit::QueryBuilder()
                .select(EntityTraits<TieringJob>::getColumns())
                .from(EntityTraits<TieringJob>::tableName())
                .where("status = ?", {jobStatusToString(JobStatus::PENDING)})
                .orderBy("created_at ASC")
                .limit(limit));
        for (const auto &row : rows) {
            TieringJob job = EntityTraits<TieringJob>::fromRow(row);
            if (_executor->execDMLWithTxn(
                    txn,
                    toolkit::QueryBuilder()
                        .update(EntityTraits<TieringJob>::tableName())
                        .set({{"status", jobStatusToString(JobStatus::RUNNING)},
                              {"updated_at", std::to_string(updated_at)}})
                        .where("job_id = ? AND status = ?",
                               {job.job_id, jobStatusToString(JobStatus::PENDING)}))) {
                job.status = jobStatusToString(JobStatus::RUNNING);
                job.updated_at = updated_at;
                claimed.push_back(job);
            }
        }
        txn->commit();
        return claimed;
    }

    void recoverRunningAt(int64_t updated_at) {
        _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<TieringJob>::tableName())
                .set({{"status", jobStatusToString(JobStatus::PENDING)},
                      {"updated_at", std::to_string(updated_at)},
                      {"error_message", "Recovered after restart"}})
                .where("status = ?", {jobStatusToString(JobStatus::RUNNING)}));
    }

    void pruneTerminalOlderThan(int64_t updated_at) {
        _executor->execDML(
            toolkit::QueryBuilder()
                .deleteFrom(EntityTraits<TieringJob>::tableName())
                .where("updated_at < ? AND status IN (?, ?, ?)",
                       {std::to_string(updated_at),
                        jobStatusToString(JobStatus::DONE),
                        jobStatusToString(JobStatus::FAILED),
                        jobStatusToString(JobStatus::CANCELLED)}));
    }
};

// ===================================================================
// Repository — SegmentTierRange
// ===================================================================
class SegmentTierRangeRepository : public SqliteRepository<SegmentTierRange> {
public:
    SegmentTierRangeRepository() : SqliteRepository<SegmentTierRange>(Database::kMediaServerDb) {}

    bool add(const SegmentTierRange &range) {
        if (range.pool_id.empty())
            return false;
        return save(range, true);
    }

    std::vector<SegmentTierRange> queryByCamera(const std::string &camera_id,
                                                int64_t start_time = 0,
                                                int64_t end_time = 0,
                                                int limit = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ?";
        params.push_back(camera_id);
        if (start_time > 0) { where << " AND end_time >= ?"; params.push_back(std::to_string(start_time)); }
        if (end_time > 0) { where << " AND start_time <= ?"; params.push_back(std::to_string(end_time)); }

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByTierAndAge(const std::string &tier,
                                                   int64_t older_than_time,
                                                   int limit = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "tier = ? AND end_time <= ? AND status = ?";
        params.push_back(tier);
        params.push_back(std::to_string(older_than_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByTierPoolAndAge(const std::string &tier,
                                                       const std::string &pool_id,
                                                       int64_t older_than_time,
                                                       int limit = 0) {
        if (pool_id.empty())
            return {};
        std::ostringstream where;
        std::vector<std::string> params;
        where << "tier = ? AND end_time <= ? AND status = ?";
        params.push_back(tier);
        params.push_back(std::to_string(older_than_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));
        where << " AND pool_id = ?";
        params.push_back(pool_id);

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByCameraTierPoolAndAge(
            const std::string &camera_id,
            const std::string &tier,
            const std::string &pool_id,
            int64_t older_than_time,
            int limit = 0) {
        if (camera_id.empty() || pool_id.empty())
            return {};
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND tier = ? AND pool_id = ?"
              << " AND end_time <= ? AND status = ?";
        params.push_back(camera_id);
        params.push_back(tier);
        params.push_back(pool_id);
        params.push_back(std::to_string(older_than_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByTierPoolStartedBefore(const std::string &tier,
                                                              const std::string &pool_id,
                                                              int64_t before_time,
                                                              int limit = 0) {
        if (pool_id.empty())
            return {};
        std::ostringstream where;
        std::vector<std::string> params;
        where << "tier = ? AND start_time < ? AND status = ? AND pool_id = ?";
        params.push_back(tier);
        params.push_back(std::to_string(before_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));
        params.push_back(pool_id);

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByTierStartedBefore(const std::string &tier,
                                                          int64_t before_time,
                                                          int limit = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "tier = ? AND start_time < ? AND status = ?";
        params.push_back(tier);
        params.push_back(std::to_string(before_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByCameraTierStartedBefore(const std::string &camera_id,
                                                                const std::string &tier,
                                                                int64_t before_time,
                                                                int limit = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND tier = ? AND start_time < ? AND status = ?";
        params.push_back(camera_id);
        params.push_back(tier);
        params.push_back(std::to_string(before_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByCameraTierAndAge(const std::string &camera_id,
                                                         const std::string &tier,
                                                         int64_t older_than_time,
                                                         int limit = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND tier = ? AND end_time <= ? AND status = ?";
        params.push_back(camera_id);
        params.push_back(tier);
        params.push_back(std::to_string(older_than_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC");
        if (limit > 0)
            query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    int countActiveByPool(const std::string &pool_id) {
        if (pool_id.empty())
            return 0;
        std::ostringstream where;
        std::vector<std::string> params;
        where << "pool_id = ? AND status NOT IN (?, ?)";
        params.push_back(pool_id);
        params.push_back(segmentStatusToString(SegmentStatus::DELETED));
        params.push_back(segmentStatusToString(SegmentStatus::EXPIRED));

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"COUNT(*)"})
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params));
        if (!rows.empty() && !rows[0].empty())
            return std::stoi(rows[0][0]);
        return 0;
    }

    std::vector<SegmentTierRange> findExpired(const std::string &camera_id = "",
                                              int64_t from_time = 0,
                                              int64_t to_time = 0,
                                              int page = 0,
                                              int size = 20) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "status = ?";
        params.push_back(segmentStatusToString(SegmentStatus::EXPIRED));
        if (!camera_id.empty()) { where << " AND camera_id = ?"; params.push_back(camera_id); }
        if (from_time > 0) { where << " AND end_time >= ?"; params.push_back(std::to_string(from_time)); }
        if (to_time > 0) { where << " AND start_time <= ?"; params.push_back(std::to_string(to_time)); }

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC")
                .limit(size)
                .offset(page * size));
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    int countExpired(const std::string &camera_id = "",
                     int64_t from_time = 0,
                     int64_t to_time = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "status = ?";
        params.push_back(segmentStatusToString(SegmentStatus::EXPIRED));
        if (!camera_id.empty()) { where << " AND camera_id = ?"; params.push_back(camera_id); }
        if (from_time > 0) { where << " AND end_time >= ?"; params.push_back(std::to_string(from_time)); }
        if (to_time > 0) { where << " AND start_time <= ?"; params.push_back(std::to_string(to_time)); }

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"COUNT(*)"})
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params));
        if (!rows.empty() && !rows[0].empty())
            return std::stoi(rows[0][0]);
        return 0;
    }

    // Appends conditions that keep a range out of deletion candidates: an
    // active transfer, an unexpired retention override or a protected video.
    // Filtering in SQL keeps undeletable rows from starving a LIMITed page.
    static void appendDeletableFilter(std::ostringstream &where,
                                      std::vector<std::string> &params,
                                      int64_t now,
                                      bool exclude_protected_video = true) {
        const std::string ranges = EntityTraits<SegmentTierRange>::tableName();
        where << " AND NOT EXISTS (SELECT 1 FROM " << EntityTraits<TieringJob>::tableName()
              << " j WHERE j.range_id = " << ranges << ".range_id AND j.status IN (?, ?))";
        params.push_back(jobStatusToString(JobStatus::PENDING));
        params.push_back(jobStatusToString(JobStatus::RUNNING));
        where << " AND NOT EXISTS (SELECT 1 FROM "
              << EntityTraits<SegmentRetentionOverride>::tableName()
              << " o WHERE o.range_id = " << ranges << ".range_id AND o.extend_until > ?)";
        params.push_back(std::to_string(now));
        if (!exclude_protected_video)
            return;
        where << " AND NOT EXISTS (SELECT 1 FROM " << EntityTraits<ProtectedVideo>::tableName()
              << " p WHERE p.camera_id = " << ranges << ".camera_id AND p.start_time <= "
              << ranges << ".end_time AND p.end_time >= " << ranges << ".start_time)";
    }

    // Oldest-first deletion candidates of one pool, paged by (end_time, range_id).
    std::vector<SegmentTierRange> findDeletableByPoolAfter(const std::string &tier,
                                                           const std::string &pool_id,
                                                           int64_t ended_before,
                                                           int64_t after_end_time,
                                                           const std::string &after_range_id,
                                                           int64_t now,
                                                           int limit) {
        std::vector<SegmentTierRange> ret;
        if (pool_id.empty())
            return ret;
        std::ostringstream where;
        std::vector<std::string> params;
        where << "tier = ? AND pool_id = ? AND status = ? AND end_time <= ?"
              << " AND (end_time > ? OR (end_time = ? AND range_id > ?))";
        params.push_back(tier);
        params.push_back(pool_id);
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));
        params.push_back(std::to_string(ended_before));
        params.push_back(std::to_string(after_end_time));
        params.push_back(std::to_string(after_end_time));
        params.push_back(after_range_id);
        // Protected videos are handled by splitting candidates around them.
        appendDeletableFilter(where, params, now, false);

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC, range_id ASC");
        if (limit > 0)
            query = query.limit(limit);
        for (const auto &row : _executor->executeRaw(query))
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    // Age-retention candidates of one camera, paged by (start_time, range_id).
    // Protected videos are not filtered here: a range is split at the cutoff
    // first, and only the old piece is checked against protections.
    std::vector<SegmentTierRange> findRetentionCandidatesAfter(const std::string &camera_id,
                                                               int64_t started_before,
                                                               int64_t after_start_time,
                                                               const std::string &after_range_id,
                                                               int64_t now,
                                                               const std::string &exclude_pool_id,
                                                               int limit) {
        std::vector<SegmentTierRange> ret;
        if (camera_id.empty())
            return ret;
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND status = ? AND start_time < ?"
              << " AND (start_time > ? OR (start_time = ? AND range_id > ?))";
        params.push_back(camera_id);
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));
        params.push_back(std::to_string(started_before));
        params.push_back(std::to_string(after_start_time));
        params.push_back(std::to_string(after_start_time));
        params.push_back(after_range_id);
        if (!exclude_pool_id.empty()) {
            where << " AND pool_id <> ?";
            params.push_back(exclude_pool_id);
        }
        appendDeletableFilter(where, params, now, false);

        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC, range_id ASC");
        if (limit > 0)
            query = query.limit(limit);
        for (const auto &row : _executor->executeRaw(query))
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    // AVAILABLE ranges strictly before the (start_time, range_id) cursor, newest first.
    std::vector<SegmentTierRange> findReconcileCandidatesBefore(int64_t before_start_time,
                                                                const std::string &before_range_id,
                                                                int limit) {
        std::vector<SegmentTierRange> ret;
        std::ostringstream where;
        std::vector<std::string> params;
        where << "status = ? AND (start_time < ?";
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));
        params.push_back(std::to_string(before_start_time));
        if (!before_range_id.empty()) {
            where << " OR (start_time = ? AND range_id < ?)";
            params.push_back(std::to_string(before_start_time));
            params.push_back(before_range_id);
        }
        where << ")";
        auto query = toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time DESC, range_id DESC");
        if (limit > 0)
            query = query.limit(limit);
        for (const auto &row : _executor->executeRaw(query))
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByRangeIds(const std::vector<std::string> &range_ids) {
        std::vector<SegmentTierRange> ret;
        std::ostringstream where;
        std::vector<std::string> params;
        for (const auto &range_id : range_ids) {
            if (range_id.empty())
                continue;
            if (!params.empty())
                where << ", ";
            where << "?";
            params.push_back(range_id);
        }
        if (params.empty())
            return ret;
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where("range_id IN (" + where.str() + ")", params));
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        std::sort(ret.begin(), ret.end(), [](const SegmentTierRange &a, const SegmentTierRange &b) {
            if (a.camera_id != b.camera_id) return a.camera_id < b.camera_id;
            return a.start_time < b.start_time;
        });
        return ret;
    }

    // True when a range of the stream owns `start_time`, i.e. start_time is in
    // its half-open [start_time, end_time) window.
    bool hasOwningRangeWithTxn(const toolkit::SqliteTransaction::Ptr &txn,
                               const std::string &camera_id,
                               const std::string &stream_id,
                               int64_t start_time) {
        if (!txn)
            return false;
        auto rows = _executor->executeRawWithTxn(
            txn,
            toolkit::QueryBuilder()
                .select({"1"})
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where("camera_id = ? AND stream_id = ? AND start_time <= ? AND end_time > ?",
                       {camera_id, stream_id, std::to_string(start_time),
                        std::to_string(start_time)})
                .limit(1));
        return !rows.empty();
    }

    // True when inserting [start_time, end_time) would take ownership of a
    // segment start already owned by another range of the stream.
    bool hasOwnershipConflictWithTxn(const toolkit::SqliteTransaction::Ptr &txn,
                                     const std::string &camera_id,
                                     const std::string &stream_id,
                                     int64_t start_time,
                                     int64_t end_time) {
        if (!txn)
            return false;
        auto rows = _executor->executeRawWithTxn(
            txn,
            toolkit::QueryBuilder()
                .select({"1"})
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where("camera_id = ? AND stream_id = ? AND start_time < ? AND end_time > ?",
                       {camera_id, stream_id, std::to_string(end_time),
                        std::to_string(start_time)})
                .limit(1));
        return !rows.empty();
    }

    bool mergeIfUncovered(const SegmentTierRange &range,
                          int64_t merge_gap_seconds,
                          int64_t updated_at) {
        auto txn = _executor->execTxn();
        try {
            if (hasOwningRangeWithTxn(txn, range.camera_id, range.stream_id,
                                      range.start_time)) {
                txn->commit();
                return true;
            }
            if (!mergeNormalizedRangeWithTxn(
                    range, merge_gap_seconds, updated_at, txn)) {
                txn->rollback();
                return false;
            }
            txn->commit();
            return true;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }

    bool mergeNormalizedRange(const SegmentTierRange &range,
                              int64_t merge_gap_seconds,
                              int64_t updated_at) {
        auto txn = _executor->execTxn();
        try {
            if (!mergeNormalizedRangeWithTxn(range, merge_gap_seconds,
                                             updated_at, txn)) {
                txn->rollback();
                return false;
            }
            txn->commit();
            return true;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }

    bool mergeNormalizedRangeWithTxn(const SegmentTierRange &range,
                                     int64_t merge_gap_seconds,
                                     int64_t updated_at,
                                     const toolkit::SqliteTransaction::Ptr &txn) {
        if (range.pool_id.empty() || !txn)
            return false;
        const int64_t bucket_start = static_cast<int64_t>(
            StampUtils::getStartOfDay(static_cast<uint64_t>(range.start_time)));
        // Noon-plus-one-day is guaranteed to fall inside the next local date,
        // including dates around daylight-saving transitions.
        const int64_t bucket_end = static_cast<int64_t>(
            StampUtils::getStartOfDay(static_cast<uint64_t>(bucket_start + 36 * 3600)));
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND tier = ? AND status = ?";
        params.push_back(range.camera_id);
        params.push_back(range.stream_id);
        params.push_back(range.tier);
        params.push_back(range.status);
        where << " AND pool_id = ?";
        params.push_back(range.pool_id);
        where << " AND end_time >= ? AND start_time <= ?";
        params.push_back(std::to_string(range.start_time - merge_gap_seconds));
        params.push_back(std::to_string(range.end_time + merge_gap_seconds));
        // Keep compact ranges inside the local calendar date of their start time.
        // This caps the amount of timeline data inspected by a split or transfer.
        where << " AND start_time >= ? AND start_time < ?";
        params.push_back(std::to_string(bucket_start));
        params.push_back(std::to_string(bucket_end));

        auto rows = _executor->executeRawWithTxn(
            txn,
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC"));
        if (rows.empty())
            return saveWithTxn(range, txn, true);

        auto existing = EntityTraits<SegmentTierRange>::fromRow(rows[0]);
        int64_t merged_start = range.start_time;
        int64_t merged_end = range.end_time;
        for (const auto &row : rows) {
            const auto item = EntityTraits<SegmentTierRange>::fromRow(row);
            merged_start = std::min(merged_start, item.start_time);
            merged_end = std::max(merged_end, item.end_time);
        }
        // Never let a merged window swallow a range of another tier, pool or
        // status lying between the candidates; keep the new range separate.
        {
            std::ostringstream other_where;
            std::vector<std::string> other_params;
            other_where << "camera_id = ? AND stream_id = ? AND start_time < ? AND end_time > ?"
                        << " AND range_id <> ?";
            other_params.push_back(range.camera_id);
            other_params.push_back(range.stream_id);
            other_params.push_back(std::to_string(merged_end));
            other_params.push_back(std::to_string(merged_start));
            other_params.push_back(range.range_id);
            for (const auto &row : rows) {
                other_where << " AND range_id <> ?";
                other_params.push_back(EntityTraits<SegmentTierRange>::fromRow(row).range_id);
            }
            auto others = _executor->executeRawWithTxn(
                txn,
                toolkit::QueryBuilder()
                    .select({"1"})
                    .from(EntityTraits<SegmentTierRange>::tableName())
                    .where(other_where.str(), other_params)
                    .limit(1));
            if (!others.empty())
                return saveWithTxn(range, txn, true);
        }

        int64_t merged_count = std::max<int64_t>(1, range.segment_count);
        int64_t merged_size = std::max<int64_t>(0, range.size_bytes);
        for (const auto &row : rows) {
            const auto item = EntityTraits<SegmentTierRange>::fromRow(row);
            merged_count += std::max<int64_t>(0, item.segment_count);
            merged_size += std::max<int64_t>(0, item.size_bytes);
            if (item.range_id != existing.range_id &&
                !_executor->execDMLWithTxn(
                    txn,
                    toolkit::QueryBuilder()
                        .deleteFrom(EntityTraits<SegmentTierRange>::tableName())
                        .where("range_id = ?", {item.range_id})))
                return false;
        }
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({
                {"start_time", std::to_string(merged_start)},
                {"end_time", std::to_string(merged_end)},
                {"segment_count", std::to_string(merged_count)},
                {"size_bytes", std::to_string(merged_size)},
                {"updated_at", std::to_string(updated_at)}
            })
            .where("range_id = ?", {existing.range_id});
        if (!_executor->execDMLWithTxn(txn, q))
            return false;
        return true;
    }

    bool normalizeRangeById(const std::string &range_id,
                            int64_t merge_gap_seconds,
                            int64_t updated_at) {
        if (range_id.empty())
            return false;
        auto txn = _executor->execTxn();
        try {
            auto rows = _executor->executeRawWithTxn(
                txn,
                toolkit::QueryBuilder()
                    .select(EntityTraits<SegmentTierRange>::getColumns())
                    .from(EntityTraits<SegmentTierRange>::tableName())
                    .where("range_id = ?", {range_id})
                    .limit(1));
            if (rows.empty()) {
                txn->rollback();
                return false;
            }
            SegmentTierRange range = EntityTraits<SegmentTierRange>::fromRow(rows.front());
            if (!_executor->execDMLWithTxn(
                    txn,
                    toolkit::QueryBuilder()
                        .deleteFrom(EntityTraits<SegmentTierRange>::tableName())
                        .where("range_id = ?", {range_id})) ||
                !mergeNormalizedRangeWithTxn(range, merge_gap_seconds,
                                             updated_at, txn)) {
                txn->rollback();
                return false;
            }
            txn->commit();
            return true;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }

    bool updateTierByWindowAt(const std::string &camera_id,
                              int64_t start_time,
                              int64_t end_time,
                              const std::string &new_tier,
                              const std::string &pool_id,
                              const std::string &status,
                              int64_t updated_at) {
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({{"tier", new_tier}, {"pool_id", pool_id}, {"status", status}, {"updated_at", std::to_string(updated_at)}})
            .where("camera_id = ? AND start_time >= ? AND end_time <= ?",
                   {camera_id, std::to_string(start_time), std::to_string(end_time)});
        return _executor->execDML(q);
    }

    bool updateTierByRangeIdAt(const std::string &range_id,
                               const std::string &new_tier,
                               const std::string &pool_id,
                               const std::string &status,
                               int64_t updated_at) {
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({{"tier", new_tier}, {"pool_id", pool_id}, {"status", status}, {"updated_at", std::to_string(updated_at)}})
            .where("range_id = ?", {range_id});
        return _executor->execDML(q);
    }

    // WHERE clause matching a range row only while it still equals the snapshot
    // a caller planned against, so concurrent split/merge/move/delete wins once.
    static const char *snapshotWhereClause() {
        return "range_id = ? AND camera_id = ? AND stream_id = ? AND "
               "start_time = ? AND end_time = ? AND tier = ? AND pool_id = ? AND "
               "status = ? AND segment_count = ? AND size_bytes = ?";
    }

    static std::vector<std::string> snapshotWhereParams(const SegmentTierRange &expected) {
        return {expected.range_id, expected.camera_id, expected.stream_id,
                std::to_string(expected.start_time), std::to_string(expected.end_time),
                expected.tier, expected.pool_id, expected.status,
                std::to_string(expected.segment_count), std::to_string(expected.size_bytes)};
    }

    // Negative segment_count/size_bytes keep the stored values.
    bool updateTierBySnapshotAt(const SegmentTierRange &expected,
                                const std::string &new_tier,
                                const std::string &pool_id,
                                const std::string &status,
                                int64_t updated_at,
                                int64_t segment_count = -1,
                                int64_t size_bytes = -1) {
        if (expected.range_id.empty() || pool_id.empty())
            return false;
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"tier", new_tier});
        set.push_back({"pool_id", pool_id});
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(updated_at)});
        if (segment_count >= 0)
            set.push_back({"segment_count", std::to_string(segment_count)});
        if (size_bytes >= 0)
            set.push_back({"size_bytes", std::to_string(size_bytes)});
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set(set)
            .where(snapshotWhereClause(), snapshotWhereParams(expected));
        return _executor->execDML(q);
    }

    bool updateStatusBySnapshotAt(const SegmentTierRange &expected,
                                  const std::string &status,
                                  int64_t updated_at) {
        return updateTierBySnapshotAt(expected, expected.tier, expected.pool_id,
                                      status, updated_at);
    }

    // Marks the range terminal and queues its metadata cleanup atomically,
    // only if the range still matches the snapshot the files were deleted from.
    bool markDeletedBySnapshotAndEnqueue(const SegmentTierRange &expected,
                                         const std::string &final_status,
                                         const TierMetadataCleanupJob &cleanup_job) {
        if (expected.range_id.empty() || cleanup_job.range_id != expected.range_id)
            return false;
        auto txn = _executor->execTxn();
        try {
            if (!_executor->execDMLWithTxn(
                    txn,
                    toolkit::QueryBuilder()
                        .update(EntityTraits<SegmentTierRange>::tableName())
                        .set({{"status", final_status},
                              {"updated_at", std::to_string(cleanup_job.updated_at)}})
                        .where(snapshotWhereClause(), snapshotWhereParams(expected)))) {
                txn->rollback();
                return false;
            }
            TierMetadataCleanupJobRepository cleanup_repo;
            if (!cleanup_repo.upsertWithTxn(cleanup_job, txn)) {
                txn->rollback();
                return false;
            }
            txn->commit();
            return true;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }

    bool updateStatusByWindowAt(const std::string &camera_id,
                                int64_t start_time,
                                int64_t end_time,
                                const std::string &status,
                                int64_t updated_at) {
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({{"status", status}, {"updated_at", std::to_string(updated_at)}})
            .where("camera_id = ? AND start_time <= ? AND end_time >= ?",
                   {camera_id, std::to_string(end_time), std::to_string(start_time)});
        return _executor->execDML(q);
    }

    bool updateStatusByRangeIdAt(const std::string &range_id,
                                 const std::string &status,
                                 int64_t updated_at) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "range_id = ?";
        params.push_back(range_id);

        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(updated_at)});
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set(set)
            .where(where.str(), params);
        return _executor->execDML(q);
    }

    bool updatePoolForTierIfMissing(const std::string &, const std::string &) { return false; }

    bool updateTierByExactWindowAt(const std::string &camera_id,
                                   const std::string &stream_id,
                                   int64_t start_time,
                                   int64_t end_time,
                                   const std::string &new_tier,
                                   const std::string &pool_id,
                                   const std::string &status,
                                   int64_t updated_at) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND start_time = ? AND end_time = ?";
        params.push_back(camera_id);
        params.push_back(stream_id);
        params.push_back(std::to_string(start_time));
        params.push_back(std::to_string(end_time));

        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"tier", new_tier});
        set.push_back({"pool_id", pool_id});
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(updated_at)});

        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set(set)
            .where(where.str(), params);
        return _executor->execDML(q);
    }

    bool updateStatusByExactWindowAt(const std::string &camera_id,
                                     const std::string &stream_id,
                                     int64_t start_time,
                                     int64_t end_time,
                                     const std::string &status,
                                     int64_t updated_at) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND start_time = ? AND end_time = ?";
        params.push_back(camera_id);
        params.push_back(stream_id);
        params.push_back(std::to_string(start_time));
        params.push_back(std::to_string(end_time));

        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(updated_at)});

        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({{"status", status}, {"updated_at", std::to_string(updated_at)}})
            .where("camera_id = ? AND stream_id = ? AND start_time = ? AND end_time = ?",
                   {camera_id, stream_id, std::to_string(start_time), std::to_string(end_time)});
        return _executor->execDML(q);
    }

    bool replaceRangeWithPieces(const std::string &range_id,
                                const std::vector<SegmentTierRange> &pieces) {
        if (range_id.empty() || pieces.empty())
            return false;

        for (const auto &piece : pieces) {
            if (piece.range_id.empty() || piece.pool_id.empty())
                return false;
        }

        auto txn = _executor->execTxn();
        if (!_executor->execDMLWithTxn(
            txn,
            toolkit::QueryBuilder()
                .deleteFrom(EntityTraits<SegmentTierRange>::tableName())
                .where("range_id = ?", {range_id})))
            return false;

        bool ok = true;
        for (const auto &piece : pieces) {
            ok = saveWithTxn(piece, txn, true) && ok;
        }
        if (ok)
            txn->commit();
        return ok;
    }

    bool replaceRangeWithPiecesIfUnchanged(
            const SegmentTierRange &expected,
            const std::vector<SegmentTierRange> &pieces) {
        if (expected.range_id.empty() || pieces.empty())
            return false;
        for (const auto &piece : pieces) {
            if (piece.range_id.empty() || piece.pool_id.empty())
                return false;
        }

        auto txn = _executor->execTxn();
        try {
            if (!_executor->execDMLWithTxn(
                    txn,
                    toolkit::QueryBuilder()
                        .deleteFrom(EntityTraits<SegmentTierRange>::tableName())
                        .where(snapshotWhereClause(), snapshotWhereParams(expected)))) {
                txn->rollback();
                return false;
            }

            std::vector<std::string> piece_ids;
            for (const auto &piece : pieces) {
                if (!saveWithTxn(piece, txn, true)) {
                    txn->rollback();
                    return false;
                }
                piece_ids.push_back(piece.range_id);
            }
            SegmentRetentionOverrideRepository override_repo;
            if (!override_repo.copyToRangesWithTxn(expected.range_id, piece_ids, txn)) {
                txn->rollback();
                return false;
            }
            txn->commit();
            return true;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }

    void pruneDeletedOlderThan(int64_t ts) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "segment_tier_ranges.updated_at < ? "
                 "AND segment_tier_ranges.status IN (?, ?) "
                 "AND NOT EXISTS ("
                 "SELECT 1 FROM tier_metadata_cleanup_jobs "
                 "WHERE tier_metadata_cleanup_jobs.range_id = segment_tier_ranges.range_id "
                 "AND tier_metadata_cleanup_jobs.status = ?)";
        // EXPIRED ranges still own files awaiting approval; never prune them.
        params.push_back(std::to_string(ts));
        params.push_back(segmentStatusToString(SegmentStatus::DELETED));
        params.push_back(segmentStatusToString(SegmentStatus::MISSING));
        params.push_back(jobStatusToString(JobStatus::PENDING));
        _executor->execDML(
            toolkit::QueryBuilder()
                .deleteFrom(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params));
    }

    std::vector<std::string> findDistinctAvailableCameras() {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "status = ?";
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"DISTINCT camera_id"})
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params));
        std::vector<std::string> ids;
        for (const auto &row : rows)
            if (!row.empty()) ids.push_back(row[0]);
        return ids;
    }
};

// ===================================================================
// Repository — PoolMetrics
// ===================================================================
class PoolMetricsRepository : public SqliteRepository<PoolMetrics> {
public:
    PoolMetricsRepository() : SqliteRepository<PoolMetrics>(Database::kMediaServerDb) {}

    bool record(const PoolMetrics &m) {
        const auto latest = latestForPool(m.pool_id);
        if (latest.timestamp == m.timestamp) {
            return _executor->execDML(
                toolkit::QueryBuilder()
                    .update(EntityTraits<PoolMetrics>::tableName())
                    .set({
                        {"used_bytes", std::to_string(m.used_bytes)},
                        {"total_bytes", std::to_string(m.total_bytes)},
                        {"usage_pct", serialize_sql_value(m.usage_pct)},
                        {"health_status", m.health_status}
                    })
                    .where("pool_id = ? AND timestamp = ?",
                           {m.pool_id, std::to_string(m.timestamp)}));
        }
        return save(m, true);
    }

    std::vector<PoolMetrics> findByPool(const std::string &pool_id,
                                        int64_t from_time, int64_t to_time,
                                        int limit = 288 /* 1 reading/5min over 24h */) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<PoolMetrics>::getColumns())
                .from(EntityTraits<PoolMetrics>::tableName())
                .where("pool_id = ? AND timestamp BETWEEN ? AND ?",
                       {pool_id, std::to_string(from_time), std::to_string(to_time)})
                .orderBy("timestamp ASC")
                .limit(limit));
        std::vector<PoolMetrics> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<PoolMetrics>::fromRow(row));
        return ret;
    }

    // Latest snapshot per pool (for dashboard)
    PoolMetrics latestForPool(const std::string &pool_id) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<PoolMetrics>::getColumns())
                .from(EntityTraits<PoolMetrics>::tableName())
                .where("pool_id = ?", {pool_id})
                .orderBy("timestamp DESC")
                .limit(1));
        if (!rows.empty())
            return EntityTraits<PoolMetrics>::fromRow(rows[0]);
        PoolMetrics m;
        m.pool_id = pool_id;
        return m;
    }

    // Prune metrics older than a given timestamp to keep DB small
    void pruneOlderThan(int64_t ts) {
        _executor->execDML(
            toolkit::QueryBuilder()
                .deleteFrom(EntityTraits<PoolMetrics>::tableName())
                .where("timestamp < ?", {std::to_string(ts)}));
    }
};

// ===================================================================
// IMP facades
// ===================================================================
class TieringJobImp : public TieringJobRepository {
public:
    using Ptr = std::shared_ptr<TieringJobImp>;
    bool add(const TieringJob &job) { return save(job, true); }

    bool updateStatus(const std::string &job_id, const std::string &status,
                      int64_t bytes_moved = -1, const std::string &error = "") {
        return updateStatusAt(job_id, status, static_cast<int64_t>(time(nullptr)),
                              bytes_moved, error);
    }

    bool transitionStatus(const std::string &job_id,
                          const std::string &from_status,
                          const std::string &to_status,
                          int64_t bytes_moved = -1,
                          const std::string &error = "") {
        return transitionStatusAt(job_id, from_status, to_status,
                                  static_cast<int64_t>(time(nullptr)),
                                  bytes_moved, error);
    }

    std::vector<TieringJob> claimPending(int limit = 100) {
        return claimPendingAt(limit, static_cast<int64_t>(time(nullptr)));
    }

    void recoverRunning() {
        recoverRunningAt(static_cast<int64_t>(time(nullptr)));
    }
};

class SegmentTierRangeImp : public SegmentTierRangeRepository {
public:
    using Ptr = std::shared_ptr<SegmentTierRangeImp>;

    bool registerIfUncovered(SegmentTierRange range,
                             int64_t merge_gap_seconds = 90) {
        if (range.pool_id.empty())
            return false;
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (range.created_at <= 0)
            range.created_at = now;
        range.updated_at = now;
        if (range.range_id.empty())
            range.range_id = StrUUID::make_guid(8, "rng");
        return mergeIfUncovered(range, merge_gap_seconds, now);
    }

    bool mergeOrInsert(SegmentTierRange range, int64_t merge_gap_seconds = 90) {
        if (range.pool_id.empty())
            return false;
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (range.created_at <= 0)
            range.created_at = now;
        range.updated_at = now;
        if (range.range_id.empty())
            range.range_id = StrUUID::make_guid(8, "rng");
        return mergeNormalizedRange(range, merge_gap_seconds, now);
    }

    bool updateTierByWindow(const std::string &camera_id, int64_t start_time,
                            int64_t end_time, const std::string &new_tier,
                            const std::string &pool_id, const std::string &status) {
        if (pool_id.empty())
            return false;
        return updateTierByWindowAt(camera_id, start_time, end_time, new_tier,
                                    pool_id, status, static_cast<int64_t>(time(nullptr)));
    }

    bool updateTierByRangeId(const std::string &range_id,
                             const std::string &new_tier,
                             const std::string &pool_id,
                             const std::string &status) {
        if (range_id.empty() || pool_id.empty())
            return false;
        return updateTierByRangeIdAt(range_id, new_tier, pool_id, status,
                                     static_cast<int64_t>(time(nullptr)));
    }

    bool updateStatusByWindow(const std::string &camera_id, int64_t start_time,
                              int64_t end_time, const std::string &status) {
        return updateStatusByWindowAt(camera_id, start_time, end_time, status,
                                      static_cast<int64_t>(time(nullptr)));
    }

    bool updateStatusByRangeId(const std::string &range_id,
                               const std::string &status) {
        if (range_id.empty())
            return false;
        return updateStatusByRangeIdAt(range_id, status,
                                       static_cast<int64_t>(time(nullptr)));
    }

    bool updateTierByExactWindow(const std::string &camera_id,
                                 const std::string &stream_id,
                                 int64_t start_time, int64_t end_time,
                                 const std::string &new_tier,
                                 const std::string &pool_id,
                                 const std::string &status) {
        if (pool_id.empty())
            return false;
        return updateTierByExactWindowAt(camera_id, stream_id, start_time,
                                         end_time, new_tier, pool_id, status,
                                         static_cast<int64_t>(time(nullptr)));
    }

    bool updateStatusByExactWindow(const std::string &camera_id,
                                   const std::string &stream_id,
                                   int64_t start_time, int64_t end_time,
                                   const std::string &status) {
        return updateStatusByExactWindowAt(camera_id, stream_id, start_time,
                                           end_time, status,
                                           static_cast<int64_t>(time(nullptr)));
    }

    bool splitWindowStatus(const SegmentTierRange &range,
                           int64_t window_start,
                           int64_t window_end,
                           const std::string &status) {
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (window_start <= range.start_time && window_end >= range.end_time) {
            return updateStatusByExactWindowAt(range.camera_id, range.stream_id,
                                               range.start_time, range.end_time,
                                               status, now);
        }

        window_start = std::max(window_start, range.start_time);
        window_end = std::min(window_end, range.end_time);
        if (range.range_id.empty() || range.pool_id.empty() || window_start >= window_end)
            return false;

        const int64_t average_size = range.segment_count > 0
                                         ? range.size_bytes / range.segment_count
                                         : 0;
        auto make_piece = [&](int64_t start, int64_t end,
                              const std::string &piece_status, bool keep_id) {
            SegmentTierRange piece = range;
            piece.range_id = keep_id ? range.range_id : StrUUID::make_guid(8, "rng");
            piece.start_time = start;
            piece.end_time = end;
            piece.status = piece_status;
            piece.segment_count = std::max<int64_t>(1, (end - start + 59) / 60);
            piece.size_bytes = average_size > 0 ? average_size * piece.segment_count : 0;
            piece.updated_at = now;
            return piece;
        };

        std::vector<SegmentTierRange> pieces;
        if (range.start_time < window_start)
            pieces.push_back(make_piece(range.start_time, window_start, range.status, false));
        pieces.push_back(make_piece(window_start, window_end, status, true));
        if (window_end < range.end_time)
            pieces.push_back(make_piece(window_end, range.end_time, range.status, false));
        return replaceRangeWithPieces(range.range_id, pieces);
    }
};

class PoolMetricsImp : public PoolMetricsRepository {
public:
    using Ptr = std::shared_ptr<PoolMetricsImp>;
};

} // namespace managerkit

#endif // STORAGE_TIERINGJOB_H

#endif // ENABLE_TIER_STORAGE
