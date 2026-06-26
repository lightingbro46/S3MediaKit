#ifndef STORAGE_TIERINGJOB_H
#define STORAGE_TIERINGJOB_H

#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/util.h"
#include "Local/StorageTier.h"

namespace managerkit {

// ===================================================================
// TieringJob entity — one row per data-movement job
// ===================================================================
struct TieringJob {
    std::string job_id;
    std::string camera_id;
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

    Json::Value toJson() const {
        Json::Value v;
        v["job_id"]              = job_id;
        v["camera_id"]           = camera_id;
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
        return v;
    }
};

DECLARE_ENTITY(TieringJob, "tiering_jobs",
    {"job_id"},
    &TieringJob::job_id,              "job_id",
    &TieringJob::camera_id,           "camera_id",
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
    &TieringJob::updated_at,          "updated_at"
)

// ===================================================================
// SegmentTierRecord entity — tracks the current tier of each segment
// ===================================================================
struct SegmentTierRecord {
    std::string camera_id;
    std::string stream_id;
    std::string segment_path;   // relative path within record_root
    std::string tier;           // HOT | WARM | COLD
    Optional<std::string> pool_id;
    std::string status;         // AVAILABLE | RESTORING | EXPIRED | DELETED | MISSING
    int64_t start_time  = 0;
    int64_t end_time    = 0;
    int64_t file_size   = 0;
    int64_t created_at  = 0;
    int64_t updated_at  = 0;

    Json::Value toJson() const {
        Json::Value v;
        v["camera_id"]     = camera_id;
        v["stream_id"]     = stream_id;
        v["segment_path"]  = segment_path;
        v["tier"]          = tier;
        v["pool_id"]       = pool_id.value_or("");
        v["status"]        = status;
        v["start_time"]    = static_cast<Json::Int64>(start_time);
        v["end_time"]      = static_cast<Json::Int64>(end_time);
        v["file_size"]     = static_cast<Json::Int64>(file_size);
        return v;
    }
};

DECLARE_ENTITY(SegmentTierRecord, "segment_tier_records",
    MAKE_PK("camera_id", "stream_id", "segment_path"),
    &SegmentTierRecord::camera_id,    "camera_id",
    &SegmentTierRecord::stream_id,    "stream_id",
    &SegmentTierRecord::segment_path, "segment_path",
    &SegmentTierRecord::tier,         "tier",
    &SegmentTierRecord::pool_id,      "pool_id",
    &SegmentTierRecord::status,       "status",
    &SegmentTierRecord::start_time,   "start_time",
    &SegmentTierRecord::end_time,     "end_time",
    &SegmentTierRecord::file_size,    "file_size",
    &SegmentTierRecord::created_at,   "created_at",
    &SegmentTierRecord::updated_at,   "updated_at"
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

    std::vector<TieringJob> findByStatus(const std::string &status) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<TieringJob>::getColumns())
                .from(EntityTraits<TieringJob>::tableName())
                .where("status = ?", {status})
                .orderBy("created_at ASC"));
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

    bool updateStatus(const std::string &job_id, const std::string &status,
                      int64_t bytes_moved = -1, const std::string &error = "") {
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(static_cast<int64_t>(time(nullptr)))});
        if (bytes_moved >= 0) set.push_back({"bytes_moved", std::to_string(bytes_moved)});
        if (!error.empty())   set.push_back({"error_message", error});

        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<TieringJob>::tableName())
            .set(set)
            .where("job_id = ?", {job_id});
        return _executor->execDML(q);
    }
};

// ===================================================================
// Repository — SegmentTierRecord
// ===================================================================
class SegmentTierRepository : public SqliteRepository<SegmentTierRecord> {
public:
    SegmentTierRepository() : SqliteRepository<SegmentTierRecord>(Database::kMediaServerDb) {}

    std::vector<SegmentTierRecord> findByCamera(const std::string &camera_id,
                                                int64_t start_time = 0,
                                                int64_t end_time   = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ?";
        params.push_back(camera_id);
        if (start_time > 0) { where << " AND start_time >= ?"; params.push_back(std::to_string(start_time)); }
        if (end_time   > 0) { where << " AND end_time <= ?";   params.push_back(std::to_string(end_time)); }

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRecord>::getColumns())
                .from(EntityTraits<SegmentTierRecord>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC"));
        std::vector<SegmentTierRecord> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRecord>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRecord> findByTierAndAge(const std::string &tier,
                                                     int64_t older_than_time) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRecord>::getColumns())
                .from(EntityTraits<SegmentTierRecord>::tableName())
                .where("tier = ? AND end_time <= ? AND status = ?",
                       {tier, std::to_string(older_than_time), "AVAILABLE"})
                .orderBy("end_time ASC"));
        std::vector<SegmentTierRecord> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRecord>::fromRow(row));
        return ret;
    }

    bool upsert(const SegmentTierRecord &rec) {
        // Delete any existing record then insert
        auto q = toolkit::QueryBuilder()
            .deleteFrom(EntityTraits<SegmentTierRecord>::tableName())
            .where("camera_id = ? AND stream_id = ? AND segment_path = ?",
                   {rec.camera_id, rec.stream_id, rec.segment_path});
        _executor->execDML(q);
        return save(rec, true);
    }

    bool updateTier(const std::string &camera_id, const std::string &stream_id,
                    const std::string &segment_path,
                    const std::string &new_tier, const std::string &pool_id,
                    const std::string &status) {
        int64_t now = static_cast<int64_t>(time(nullptr));
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRecord>::tableName())
            .set({{"tier", new_tier}, {"pool_id", pool_id},
                  {"status", status}, {"updated_at", std::to_string(now)}})
            .where("camera_id = ? AND stream_id = ? AND segment_path = ?",
                   {camera_id, stream_id, segment_path});
        return _executor->execDML(q);
    }
};

// ===================================================================
// Repository — PoolMetrics
// ===================================================================
class PoolMetricsRepository : public SqliteRepository<PoolMetrics> {
public:
    PoolMetricsRepository() : SqliteRepository<PoolMetrics>(Database::kMediaServerDb) {}

    bool record(const PoolMetrics &m) { return save(m, true); }

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
};

class SegmentTierImp : public SegmentTierRepository {
public:
    using Ptr = std::shared_ptr<SegmentTierImp>;

    // Return distinct camera_ids that have AVAILABLE segments
    std::vector<std::string> findDistinctAvailableCameras() {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"DISTINCT camera_id"})
                .from(EntityTraits<SegmentTierRecord>::tableName())
                .where("status = ?", {segmentStatusToString(SegmentStatus::AVAILABLE)}));
        std::vector<std::string> ids;
        for (const auto &row : rows)
            if (!row.empty()) ids.push_back(row[0]);
        return ids;
    }
};

class PoolMetricsImp : public PoolMetricsRepository {
public:
    using Ptr = std::shared_ptr<PoolMetricsImp>;
};

} // namespace managerkit

#endif // STORAGE_TIERINGJOB_H
