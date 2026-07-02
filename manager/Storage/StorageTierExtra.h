#ifndef STORAGE_TIER_EXTRA_H
#define STORAGE_TIER_EXTRA_H

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

    std::vector<RestoreJob> queryExpiredDone(int64_t cutoff_updated_at,
                                             int size = 100) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<RestoreJob>::getColumns())
                .from(EntityTraits<RestoreJob>::tableName())
                .where("status = ? AND updated_at > 0 AND updated_at <= ?",
                       {"DONE", std::to_string(cutoff_updated_at)})
                .orderBy("updated_at ASC")
                .limit(size));

        std::vector<RestoreJob> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<RestoreJob>::fromRow(row));
        return ret;
    }

    bool updateStatus(const std::string &job_id,
                      const std::string &status,
                      int64_t processed_bytes = -1,
                      const std::string &error = "") {
        std::vector<std::pair<std::string, std::string>> set;
        set.push_back({"status", status});
        set.push_back({"updated_at", std::to_string(static_cast<int64_t>(time(nullptr)))});
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

using RestoreJobImp = RestoreJobRepository;
using StorageAlertImp = StorageAlertRepository;
using ProtectedVideoImp = ProtectedVideoRepository;

} // namespace managerkit

#endif // STORAGE_TIER_EXTRA_H
