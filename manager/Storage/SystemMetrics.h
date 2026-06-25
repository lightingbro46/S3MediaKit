#ifndef STORAGE_SYSTEMMETRICS_H
#define STORAGE_SYSTEMMETRICS_H

#include <string>
#include <vector>
#include "json/json.h"
#include "DbStorage.h"
#include "Util/util.h"

namespace managerkit {

struct SystemMetric {
    int64_t id               = 0;
    int64_t timestamp        = 0;   // unix epoch seconds
    float   cpu_usage_pct    = 0.f;
    float   cpu_proc_usage_pct = 0.f;
    int     cpu_cores        = 0;
    int64_t ram_used         = 0;
    int64_t ram_total        = 0;
    float   ram_usage_pct    = 0.f;
    int     reader_total     = 0;
    int     reader_live      = 0;
    int     reader_playback  = 0;
    std::string nets_json    = "[]";
    std::string disks_json   = "[]";

    Json::Value toJson() const {
        Json::Value v;
        v["id"]                = (Json::Int64)id;
        v["timestamp"]         = (Json::Int64)timestamp;
        v["cpu_usage_pct"]     = cpu_usage_pct;
        v["cpu_proc_usage_pct"]= cpu_proc_usage_pct;
        v["cpu_cores"]         = cpu_cores;
        v["ram_used"]          = (Json::UInt64)ram_used;
        v["ram_total"]         = (Json::UInt64)ram_total;
        v["ram_usage_pct"]     = ram_usage_pct;
        v["reader_total"]      = reader_total;
        v["reader_live"]       = reader_live;
        v["reader_playback"]   = reader_playback;
        // Embed nested JSON objects parsed from stored text
        Json::Reader reader;
        Json::Value nets_val;
        if (reader.parse(nets_json, nets_val)) v["nets"] = nets_val;
        else                                   v["nets"] = Json::arrayValue;
        Json::Value disks_val;
        if (reader.parse(disks_json, disks_val)) v["disks"] = disks_val;
        else                                     v["disks"] = Json::arrayValue;
        return v;
    }
};

DECLARE_ENTITY(SystemMetric, "system_metrics",
    {"id"},
    &SystemMetric::id,                "id",
    &SystemMetric::timestamp,         "timestamp",
    &SystemMetric::cpu_usage_pct,     "cpu_usage_pct",
    &SystemMetric::cpu_proc_usage_pct,"cpu_proc_usage_pct",
    &SystemMetric::cpu_cores,         "cpu_cores",
    &SystemMetric::ram_used,          "ram_used",
    &SystemMetric::ram_total,         "ram_total",
    &SystemMetric::ram_usage_pct,     "ram_usage_pct",
    &SystemMetric::reader_total,      "reader_total",
    &SystemMetric::reader_live,       "reader_live",
    &SystemMetric::reader_playback,   "reader_playback",
    &SystemMetric::nets_json,         "nets_json",
    &SystemMetric::disks_json,        "disks_json"
)

class SystemMetricsRepository : public SqliteRepository<SystemMetric> {
public:
    SystemMetricsRepository() : SqliteRepository<SystemMetric>(Database::kMediaServerDb) {}

    /**
     * Insert one metric sample.
     */
    void insertMetric(const SystemMetric &m) {
        save(m);
    }

    /**
     * Delete samples older than the given unix-epoch timestamp (seconds).
     */
    void deleteOlderThan(int64_t before_timestamp) {
        auto query = toolkit::QueryBuilder()
            .deleteFrom(EntityTraits<SystemMetric>::tableName())
            .where("timestamp < ?", { serialize_sql_value(before_timestamp) });
        _executor->execDML(query);
    }

    /**
     * Query samples in [from_ts, to_ts] inclusive, ordered by timestamp ASC.
     * Pass 0 for to_ts to mean "no upper bound".
     */
    std::vector<SystemMetric> findByTimeRange(int64_t from_ts, int64_t to_ts, int limit = 0) {
        std::ostringstream where;
        std::vector<std::string> params;
        where << "timestamp >= ?";
        params.push_back(serialize_sql_value(from_ts));
        if (to_ts > 0) {
            where << " AND timestamp <= ?";
            params.push_back(serialize_sql_value(to_ts));
        }
        auto query = toolkit::QueryBuilder()
            .select(EntityTraits<SystemMetric>::getColumns())
            .from(EntityTraits<SystemMetric>::tableName())
            .where(where.str(), params)
            .orderBy("timestamp ASC");
        if (limit > 0) query = query.limit(limit);
        auto rows = _executor->executeRaw(query);
        std::vector<SystemMetric> ret;
        ret.reserve(rows.size());
        for (const auto &row : rows)
            ret.push_back(EntityTraits<SystemMetric>::fromRow(row));
        return ret;
    }
};

class SystemMetricsImp : public SystemMetricsRepository {
public:
    using Ptr = std::shared_ptr<SystemMetricsImp>;
};

} // namespace managerkit

#endif // STORAGE_SYSTEMMETRICS_H
