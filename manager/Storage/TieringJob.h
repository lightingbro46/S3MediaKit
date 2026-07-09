#ifndef STORAGE_TIERINGJOB_H
#define STORAGE_TIERINGJOB_H

#include <algorithm>
#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/util.h"
#include "Common/StrUtil.h"
#include "Local/StorageTier.h"

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
    std::string pool_id;
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
        v["pool_id"]       = pool_id;
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

    std::vector<TieringJob> findByStatus(const std::string &status) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "status = ?";
        params.push_back(status);

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<TieringJob>::getColumns())
                .from(EntityTraits<TieringJob>::tableName())
                .where(where.str(), params)
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

    bool exists(const std::string &camera_id,
                const std::string &stream_id,
                const std::string &segment_path) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND segment_path = ?";
        params.push_back(camera_id);
        params.push_back(stream_id);
        params.push_back(segment_path);
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"1"})
                .from(EntityTraits<SegmentTierRecord>::tableName())
                .where(where.str(), params)
                .limit(1));
        return !rows.empty();
    }

    std::vector<SegmentTierRecord> findByTierAndAge(const std::string &tier,
                                                     int64_t older_than_time) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "tier = ? AND end_time <= ? AND status = ?";
        params.push_back(tier);
        params.push_back(std::to_string(older_than_time));
        params.push_back("AVAILABLE");
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRecord>::getColumns())
                .from(EntityTraits<SegmentTierRecord>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC"));
        std::vector<SegmentTierRecord> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRecord>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRecord> findExpired(const std::string &camera_id = "",
                                               int64_t from_time = 0,
                                               int64_t to_time = 0,
                                               int page = 0,
                                               int size = 20) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "status = ?";
        params.push_back(segmentStatusToString(SegmentStatus::EXPIRED));
        if (!camera_id.empty()) { where << " AND camera_id = ?"; params.push_back(camera_id); }
        if (from_time > 0) { where << " AND start_time >= ?"; params.push_back(std::to_string(from_time)); }
        if (to_time > 0) { where << " AND end_time <= ?"; params.push_back(std::to_string(to_time)); }

        auto q = toolkit::QueryBuilder()
            .select(EntityTraits<SegmentTierRecord>::getColumns())
            .from(EntityTraits<SegmentTierRecord>::tableName())
            .where(where.str(), params)
            .orderBy("end_time ASC")
            .limit(size)
            .offset(page * size);

        auto rows = _executor->executeRaw(q);
        std::vector<SegmentTierRecord> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRecord>::fromRow(row));
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
        if (from_time > 0) { where << " AND start_time >= ?"; params.push_back(std::to_string(from_time)); }
        if (to_time > 0) { where << " AND end_time <= ?"; params.push_back(std::to_string(to_time)); }

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"COUNT(*)"})
                .from(EntityTraits<SegmentTierRecord>::tableName())
                .where(where.str(), params));
        if (!rows.empty() && !rows[0].empty())
            return std::stoi(rows[0][0]);
        return 0;
    }

    bool upsert(const SegmentTierRecord &rec) {
        if (rec.pool_id.empty())
            return false;
        // Delete any existing record then insert
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND segment_path = ?";
        params.push_back(rec.camera_id);
        params.push_back(rec.stream_id);
        params.push_back(rec.segment_path);
        auto q = toolkit::QueryBuilder()
            .deleteFrom(EntityTraits<SegmentTierRecord>::tableName())
            .where(where.str(), params);
        _executor->execDML(q);
        return save(rec, true);
    }

    bool updateTier(const std::string &camera_id, const std::string &stream_id,
                    const std::string &segment_path,
                    const std::string &new_tier, const std::string &pool_id,
                    const std::string &status) {
        if (pool_id.empty())
            return false;
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND segment_path = ?";
        params.push_back(camera_id);
        params.push_back(stream_id);
        params.push_back(segment_path);

        int64_t now = static_cast<int64_t>(time(nullptr));
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"tier", new_tier});
        set.push_back({"pool_id", pool_id});
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(now)});
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRecord>::tableName())
            .set(set)
            .where(where.str(), params);
        return _executor->execDML(q);
    }

    bool updateStatus(const std::string &camera_id, const std::string &stream_id,
                      const std::string &segment_path,
                      const std::string &status) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND segment_path = ?";
        params.push_back(camera_id);
        params.push_back(stream_id);
        params.push_back(segment_path);

        int64_t now = static_cast<int64_t>(time(nullptr));
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(now)});
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRecord>::tableName())
            .set(set)
            .where(where.str(), params);
        return _executor->execDML(q);
    }

    void pruneOlderThan(int64_t ts) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "updated_at < ?";
        params.push_back(std::to_string(ts));
        
        auto q = toolkit::QueryBuilder()
            .deleteFrom(EntityTraits<SegmentTierRecord>::tableName())
            .where(where.str(), params);

        _executor->execDML(q);
    }

    // Return distinct camera_ids that have AVAILABLE segments
    std::vector<std::string> findDistinctAvailableCameras() {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "status = ?";
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));

        auto q = toolkit::QueryBuilder()
                .select({"DISTINCT camera_id"})
                .from(EntityTraits<SegmentTierRecord>::tableName())
                .where(where.str(), params);
        auto rows = _executor->executeRaw(q);
        std::vector<std::string> ids;
        for (const auto &row : rows)
            if (!row.empty()) ids.push_back(row[0]);
        return ids;
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
                                                int64_t end_time = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ?";
        params.push_back(camera_id);
        if (start_time > 0) { where << " AND end_time >= ?"; params.push_back(std::to_string(start_time)); }
        if (end_time > 0) { where << " AND start_time <= ?"; params.push_back(std::to_string(end_time)); }

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC"));
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByTierAndAge(const std::string &tier,
                                                   int64_t older_than_time) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "tier = ? AND end_time <= ? AND status = ?";
        params.push_back(tier);
        params.push_back(std::to_string(older_than_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC"));
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByTierPoolAndAge(const std::string &tier,
                                                       const std::string &pool_id,
                                                       int64_t older_than_time) {
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

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("end_time ASC"));
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
    }

    std::vector<SegmentTierRange> findByTierPoolStartedBefore(const std::string &tier,
                                                              const std::string &pool_id,
                                                              int64_t before_time) {
        if (pool_id.empty())
            return {};
        std::ostringstream where;
        std::vector<std::string> params;
        where << "tier = ? AND start_time < ? AND status = ? AND pool_id = ?";
        params.push_back(tier);
        params.push_back(std::to_string(before_time));
        params.push_back(segmentStatusToString(SegmentStatus::AVAILABLE));
        params.push_back(pool_id);

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time ASC"));
        std::vector<SegmentTierRange> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SegmentTierRange>::fromRow(row));
        return ret;
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

    std::vector<SegmentTierRange> findByRangeIds(const std::vector<std::string> &range_ids) {
        std::vector<SegmentTierRange> ret;
        for (const auto &range_id : range_ids) {
            if (range_id.empty())
                continue;
            SegmentTierRange key;
            key.range_id = range_id;
            auto rows = SqliteRepository<SegmentTierRange>::findById(key);
            ret.insert(ret.end(), rows.begin(), rows.end());
        }
        std::sort(ret.begin(), ret.end(), [](const SegmentTierRange &a, const SegmentTierRange &b) {
            if (a.camera_id != b.camera_id) return a.camera_id < b.camera_id;
            return a.start_time < b.start_time;
        });
        return ret;
    }

    bool hasCoveringRange(const std::string &camera_id,
                          const std::string &stream_id,
                          const std::string &tier,
                          const std::string &status,
                          int64_t start_time,
                          int64_t end_time) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND tier = ? AND status = ? AND start_time <= ? AND end_time >= ?";
        params.push_back(camera_id);
        params.push_back(stream_id);
        params.push_back(tier);
        params.push_back(status);
        params.push_back(std::to_string(start_time));
        params.push_back(std::to_string(end_time));

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select({"1"})
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .limit(1));
        return !rows.empty();
    }

    bool mergeOrInsert(SegmentTierRange range, int64_t merge_gap_seconds = 90) {
        if (range.pool_id.empty())
            return false;
        int64_t now = static_cast<int64_t>(time(nullptr));
        if (range.created_at <= 0) range.created_at = now;
        range.updated_at = now;
        if (range.range_id.empty())
            range.range_id = StrUUID::make_guid(8, "rng");

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

        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<SegmentTierRange>::getColumns())
                .from(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params)
                .orderBy("start_time DESC")
                .limit(1));
        if (rows.empty()) {
            return save(range, true);
        }

        auto existing = EntityTraits<SegmentTierRange>::fromRow(rows[0]);
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({
                {"start_time", std::to_string(std::min(existing.start_time, range.start_time))},
                {"end_time", std::to_string(std::max(existing.end_time, range.end_time))},
                {"segment_count", std::to_string(existing.segment_count + std::max<int64_t>(1, range.segment_count))},
                {"size_bytes", std::to_string(existing.size_bytes + std::max<int64_t>(0, range.size_bytes))},
                {"updated_at", std::to_string(now)}
            })
            .where("range_id = ?", {existing.range_id});
        return _executor->execDML(q);
    }

    bool updateTierByWindow(const std::string &camera_id,
                            int64_t start_time,
                            int64_t end_time,
                            const std::string &new_tier,
                            const std::string &pool_id,
                            const std::string &status) {
        if (pool_id.empty())
            return false;
        int64_t now = static_cast<int64_t>(time(nullptr));
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({{"tier", new_tier}, {"pool_id", pool_id}, {"status", status}, {"updated_at", std::to_string(now)}})
            .where("camera_id = ? AND start_time >= ? AND end_time <= ?",
                   {camera_id, std::to_string(start_time), std::to_string(end_time)});
        return _executor->execDML(q);
    }

    bool updateTierByRangeId(const std::string &range_id,
                             const std::string &new_tier,
                             const std::string &pool_id,
                             const std::string &status) {
        if (range_id.empty() || pool_id.empty())
            return false;
        int64_t now = static_cast<int64_t>(time(nullptr));
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({{"tier", new_tier}, {"pool_id", pool_id}, {"status", status}, {"updated_at", std::to_string(now)}})
            .where("range_id = ?", {range_id});
        return _executor->execDML(q);
    }

    bool updateStatusByWindow(const std::string &camera_id,
                              int64_t start_time,
                              int64_t end_time,
                              const std::string &status) {
        int64_t now = static_cast<int64_t>(time(nullptr));
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({{"status", status}, {"updated_at", std::to_string(now)}})
            .where("camera_id = ? AND start_time <= ? AND end_time >= ?",
                   {camera_id, std::to_string(end_time), std::to_string(start_time)});
        return _executor->execDML(q);
    }

    bool updateStatusByRangeId(const std::string &range_id,
                               const std::string &status) {
        int64_t now = static_cast<int64_t>(time(nullptr));
        std::ostringstream where;
        std::vector<std::string> params;
        where << "range_id = ?";
        params.push_back(range_id);

        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(now)});
        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set(set)
            .where(where.str(), params);
        return _executor->execDML(q);
    }

    bool updatePoolForTierIfMissing(const std::string &, const std::string &) { return false; }

    bool updateTierByExactWindow(const std::string &camera_id,
                                 const std::string &stream_id,
                                 int64_t start_time,
                                 int64_t end_time,
                                 const std::string &new_tier,
                                 const std::string &pool_id,
                                 const std::string &status) {
        if (pool_id.empty())
            return false;
        int64_t now = static_cast<int64_t>(time(nullptr));
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
        set.push_back({"updated_at", std::to_string(now)});

        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set(set)
            .where(where.str(), params);
        return _executor->execDML(q);
    }

    bool updateStatusByExactWindow(const std::string &camera_id,
                                   const std::string &stream_id,
                                   int64_t start_time,
                                   int64_t end_time,
                                   const std::string &status) {
        int64_t now = static_cast<int64_t>(time(nullptr));
        std::ostringstream where;
        std::vector<std::string> params;
        where << "camera_id = ? AND stream_id = ? AND start_time = ? AND end_time = ?";
        params.push_back(camera_id);
        params.push_back(stream_id);
        params.push_back(std::to_string(start_time));
        params.push_back(std::to_string(end_time));

        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(now)});

        auto q = toolkit::QueryBuilder()
            .update(EntityTraits<SegmentTierRange>::tableName())
            .set({{"status", status}, {"updated_at", std::to_string(now)}})
            .where("camera_id = ? AND stream_id = ? AND start_time = ? AND end_time = ?",
                   {camera_id, stream_id, std::to_string(start_time), std::to_string(end_time)});
        return _executor->execDML(q);
    }

    bool splitWindowStatus(const SegmentTierRange &range,
                           int64_t window_start,
                           int64_t window_end,
                           const std::string &status) {
        if (window_start <= range.start_time && window_end >= range.end_time) {
            return updateStatusByExactWindow(range.camera_id, range.stream_id,
                                             range.start_time, range.end_time, status);
        }

        window_start = std::max(window_start, range.start_time);
        window_end = std::min(window_end, range.end_time);
        if (window_start >= window_end)
            return false;

        int64_t now = static_cast<int64_t>(time(nullptr));
        int64_t avg_size = range.segment_count > 0 ? range.size_bytes / range.segment_count : 0;
        auto make_piece = [&](int64_t start, int64_t end, const std::string &piece_status, bool keep_id) {
            SegmentTierRange piece = range;
            piece.range_id = keep_id ? range.range_id : ("rng-" + toolkit::format_guid(toolkit::strToLower(toolkit::makeRandStr(32))).substr(0, 8));
            piece.start_time = start;
            piece.end_time = end;
            piece.status = piece_status;
            piece.segment_count = std::max<int64_t>(1, (end - start + 59) / 60);
            piece.size_bytes = avg_size > 0 ? avg_size * piece.segment_count : 0;
            piece.updated_at = now;
            return piece;
        };

        std::ostringstream where;
        std::vector<std::string> params;
        where << "range_id = ?";
        params.push_back(range.range_id);

        _executor->execDML(
            toolkit::QueryBuilder()
                .deleteFrom(EntityTraits<SegmentTierRange>::tableName())
                .where(where.str(), params));

        bool ok = true;
        if (range.start_time < window_start)
            ok = save(make_piece(range.start_time, window_start, range.status, false), true) && ok;
        ok = save(make_piece(window_start, window_end, status, true), true) && ok;
        if (window_end < range.end_time)
            ok = save(make_piece(window_end, range.end_time, range.status, false), true) && ok;
        return ok;
    }

    bool replaceRangeWithPieces(const std::string &range_id,
                                const std::vector<SegmentTierRange> &pieces) {
        if (range_id.empty() || pieces.empty())
            return false;

        _executor->execDML(
            toolkit::QueryBuilder()
                .deleteFrom(EntityTraits<SegmentTierRange>::tableName())
                .where("range_id = ?", {range_id}));

        bool ok = true;
        for (const auto &piece : pieces) {
            if (piece.range_id.empty() || piece.pool_id.empty())
                return false;
            ok = save(piece, true) && ok;
        }
        return ok;
    }

    void pruneDeletedOlderThan(int64_t ts) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "updated_at < ? AND status IN (?, ?)";
        params.push_back(std::to_string(ts));
        params.push_back(segmentStatusToString(SegmentStatus::DELETED));
        params.push_back(segmentStatusToString(SegmentStatus::EXPIRED));
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

};

class SegmentTierRangeImp : public SegmentTierRangeRepository {
public:
    using Ptr = std::shared_ptr<SegmentTierRangeImp>;

};

class PoolMetricsImp : public PoolMetricsRepository {
public:
    using Ptr = std::shared_ptr<PoolMetricsImp>;
};

} // namespace managerkit

#endif // STORAGE_TIERINGJOB_H
