#ifndef STORAGE_HEALTHMETRICS_H
#define STORAGE_HEALTHMETRICS_H

#include <string>
#include <vector>
#include "json/json.h"
#include "DbStorage.h"
#include "Util/util.h"

namespace managerkit {

// ──────────────────────────────────────────────────────────────────
// health_metrics: one evaluated health sample (migration 23)
// ──────────────────────────────────────────────────────────────────
struct HealthMetric {
    int64_t id = 0;
    int64_t timestamp = 0;           // unix epoch seconds
    std::string overall = "unknown";
    int score = 0;
    int cameras_total = 0;
    int cameras_online = 0;
    int rec_expected = 0;
    int rec_active = 0;
    int rec_stalled = 0;
    int viewers_total = 0;
    float poller_delay_max_ms = 0;
    int alerts_active = 0;
    int64_t proc_rss = 0;            // bytes
    std::string components_json = "{}";
};

DECLARE_ENTITY(HealthMetric, "health_metrics",
    {"id"},
    &HealthMetric::id,                  "id",
    &HealthMetric::timestamp,           "timestamp",
    &HealthMetric::overall,             "overall",
    &HealthMetric::score,               "score",
    &HealthMetric::cameras_total,       "cameras_total",
    &HealthMetric::cameras_online,      "cameras_online",
    &HealthMetric::rec_expected,        "rec_expected",
    &HealthMetric::rec_active,          "rec_active",
    &HealthMetric::rec_stalled,         "rec_stalled",
    &HealthMetric::viewers_total,       "viewers_total",
    &HealthMetric::poller_delay_max_ms, "poller_delay_max_ms",
    &HealthMetric::alerts_active,       "alerts_active",
    &HealthMetric::proc_rss,            "proc_rss",
    &HealthMetric::components_json,     "components_json"
)

class HealthMetricsRepository : public SqliteRepository<HealthMetric> {
public:
    HealthMetricsRepository() : SqliteRepository<HealthMetric>(Database::kMediaServerDb) {}

    bool insertMetric(const HealthMetric &m) { return save(m); }

    void deleteOlderThan(int64_t before_timestamp) {
        auto query = toolkit::QueryBuilder()
            .deleteFrom(EntityTraits<HealthMetric>::tableName())
            .where("timestamp < ?", { serialize_sql_value(before_timestamp) });
        _executor->execDML(query);
    }

    // Samples in [from_ts, to_ts] (to_ts = 0: no upper bound), oldest first.
    std::vector<HealthMetric> findByTimeRange(int64_t from_ts, int64_t to_ts, int limit = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "timestamp >= ?";
        params.push_back(serialize_sql_value(from_ts));
        if (to_ts > 0) {
            where << " AND timestamp <= ?";
            params.push_back(serialize_sql_value(to_ts));
        }
        auto query = toolkit::QueryBuilder()
            .select(EntityTraits<HealthMetric>::getColumns())
            .from(EntityTraits<HealthMetric>::tableName())
            .where(where.str(), params)
            .orderBy("timestamp ASC");
        if (limit > 0) query = query.limit(limit);
        std::vector<HealthMetric> ret;
        for (const auto &row : _executor->executeRaw(query))
            ret.push_back(EntityTraits<HealthMetric>::fromRow(row));
        return ret;
    }

    // (timestamp, proc_rss) since `from_ts`, oldest first: the RSS growth check reads only these.
    std::vector<std::pair<int64_t, int64_t>> findRssSince(int64_t from_ts) {
        auto query = toolkit::QueryBuilder()
            .select({"timestamp", "proc_rss"})
            .from(EntityTraits<HealthMetric>::tableName())
            .where("timestamp >= ? AND proc_rss > 0", { serialize_sql_value(from_ts) })
            .orderBy("timestamp ASC");
        std::vector<std::pair<int64_t, int64_t>> ret;
        for (const auto &row : _executor->executeRaw(query)) {
            if (row.size() >= 2)
                ret.emplace_back(std::stoll(row[0]), std::stoll(row[1]));
        }
        return ret;
    }
};

class HealthMetricsImp : public HealthMetricsRepository {
public:
    using Ptr = std::shared_ptr<HealthMetricsImp>;
};

// ──────────────────────────────────────────────────────────────────
// process_starts: one row per MediaServer start
// ──────────────────────────────────────────────────────────────────
struct ProcessStart {
    int64_t id = 0;
    int64_t started_at = 0;
    int64_t stopped_at = 0;   // clean exit time, 0 = still running or not ended cleanly
    int prev_clean = -1;      // run before this one: 1 clean exit, 0 not, -1 none
    std::string version;
};

DECLARE_ENTITY(ProcessStart, "process_starts",
    {"id"},
    &ProcessStart::id,         "id",
    &ProcessStart::started_at, "started_at",
    &ProcessStart::stopped_at, "stopped_at",
    &ProcessStart::prev_clean, "prev_clean",
    &ProcessStart::version,    "version"
)

class ProcessStartRepository : public SqliteRepository<ProcessStart> {
public:
    ProcessStartRepository() : SqliteRepository<ProcessStart>(Database::kMediaServerDb) {}

protected:
    std::vector<ProcessStart> findLatestWithTxn(const toolkit::SqliteTransaction::Ptr &txn) {
        auto query = toolkit::QueryBuilder()
            .select(EntityTraits<ProcessStart>::getColumns())
            .from(EntityTraits<ProcessStart>::tableName())
            .orderBy("id DESC")
            .limit(1);
        std::vector<ProcessStart> ret;
        for (const auto &row : _executor->executeRawWithTxn(txn, query))
            ret.push_back(EntityTraits<ProcessStart>::fromRow(row));
        return ret;
    }

    bool insertWithTxn(const ProcessStart &s, const toolkit::SqliteTransaction::Ptr &txn) {
        auto query = toolkit::QueryBuilder()
            .insertInto(EntityTraits<ProcessStart>::tableName())
            .values({ { "started_at", serialize_sql_value(s.started_at) },
                      { "stopped_at", serialize_sql_value(s.stopped_at) },
                      { "prev_clean", serialize_sql_value(s.prev_clean) },
                      { "version", s.version } });
        return _executor->execDMLWithTxn(txn, query);
    }

public:
    bool setStoppedAt(int64_t id, int64_t stopped_at) {
        auto query = toolkit::QueryBuilder()
            .update(EntityTraits<ProcessStart>::tableName())
            .set({ { "stopped_at", serialize_sql_value(stopped_at) } })
            .where("id = ?", { serialize_sql_value(id) });
        return _executor->execDML(query);
    }

    // Starts with a run before them (restarts) since `from_ts`; `unclean` = those whose previous run
    // did not exit cleanly.
    void countRestartsSince(int64_t from_ts, int &restarts, int &unclean) {
        restarts = unclean = 0;
        auto query = toolkit::QueryBuilder()
            .select({"COUNT(*)", "COALESCE(SUM(CASE WHEN prev_clean = 0 THEN 1 ELSE 0 END), 0)"})
            .from(EntityTraits<ProcessStart>::tableName())
            .where("started_at >= ? AND prev_clean >= 0", { serialize_sql_value(from_ts) });
        auto rows = _executor->executeRaw(query);
        if (!rows.empty() && rows[0].size() >= 2) {
            restarts = std::stoi(rows[0][0]);
            unclean = std::stoi(rows[0][1]);
        }
    }

    void deleteOlderThan(int64_t before_timestamp) {
        auto query = toolkit::QueryBuilder()
            .deleteFrom(EntityTraits<ProcessStart>::tableName())
            .where("started_at < ?", { serialize_sql_value(before_timestamp) });
        _executor->execDML(query);
    }
};

class ProcessStartImp : public ProcessStartRepository {
public:
    using Ptr = std::shared_ptr<ProcessStartImp>;

    /**
     * Records this process start and returns its row id (0 on failure). The previous row tells
     * whether the run before ended cleanly: stopped_at is only written by a clean exit.
     */
    int64_t recordStart(int64_t started_at, const std::string &version) {
        auto txn = _executor->beginWriteTxn();
        auto last = findLatestWithTxn(txn);
        ProcessStart s;
        s.started_at = started_at;
        s.prev_clean = last.empty() ? -1 : (last.front().stopped_at > 0 ? 1 : 0);
        s.version = version;
        if (!insertWithTxn(s, txn)) {
            return 0;
        }
        auto row = findLatestWithTxn(txn);
        txn->commit();
        return row.empty() ? 0 : row.front().id;
    }
};

// ──────────────────────────────────────────────────────────────────
// stream_baselines: fps/bitrate baseline per camera stream
// ──────────────────────────────────────────────────────────────────
struct StreamBaselineRow {
    std::string camera_id;
    int stream_type = 0;
    std::string format;
    float fps = 0;
    float bitrate_kbps = 0;
    int64_t learnt_at = 0;
};

DECLARE_ENTITY(StreamBaselineRow, "stream_baselines",
    std::vector<std::string>({"camera_id", "stream_type"}),
    &StreamBaselineRow::camera_id,    "camera_id",
    &StreamBaselineRow::stream_type,  "stream_type",
    &StreamBaselineRow::format,       "format",
    &StreamBaselineRow::fps,          "fps",
    &StreamBaselineRow::bitrate_kbps, "bitrate_kbps",
    &StreamBaselineRow::learnt_at,    "learnt_at"
)

class StreamBaselineRepository : public SqliteRepository<StreamBaselineRow> {
public:
    StreamBaselineRepository() : SqliteRepository<StreamBaselineRow>(Database::kMediaServerDb) {}

    std::vector<StreamBaselineRow> findAll() {
        auto query = toolkit::QueryBuilder()
            .select(EntityTraits<StreamBaselineRow>::getColumns())
            .from(EntityTraits<StreamBaselineRow>::tableName());
        std::vector<StreamBaselineRow> ret;
        for (const auto &row : _executor->executeRaw(query))
            ret.push_back(EntityTraits<StreamBaselineRow>::fromRow(row));
        return ret;
    }

    bool removeStream(const std::string &camera_id, int stream_type) {
        StreamBaselineRow key;
        key.camera_id = camera_id;
        key.stream_type = stream_type;
        return removeById(key);
    }

protected:
    bool insertRow(const StreamBaselineRow &row) { return save(row, true); }
};

class StreamBaselineImp : public StreamBaselineRepository {
public:
    using Ptr = std::shared_ptr<StreamBaselineImp>;

    // Insert or replace the baseline of one stream.
    bool upsert(const StreamBaselineRow &row) {
        removeStream(row.camera_id, row.stream_type);
        return insertRow(row);
    }
};

} // namespace managerkit

#endif // STORAGE_HEALTHMETRICS_H
