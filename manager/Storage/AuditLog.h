#ifndef STORAGE_AUDIT_LOG_H
#define STORAGE_AUDIT_LOG_H

#include "Common/config.h"
#include "DbStorage.h"
#include "Util/util.h"

namespace managerkit {

struct AuditLog {
    int id;
    uint64_t created_time_sec;
    uint64_t range_start_sec;
    uint64_t range_end_sec;
    int event_type;
    std::string resources;
    std::string params;
    std::string auth_session;

    Json::Value toJson() const {
        Json::Value v;
        v["id"] = id;
        v["createdTimeSec"] = (Json::UInt64)created_time_sec;
        v["rangeStartSec"] = (Json::UInt64)range_start_sec;
        v["rangeEndSec"] = (Json::UInt64)range_end_sec;
        v["eventType"] = event_type;
        v["resources"] = resources;
        v["params"] = params;
        v["authSession"] = auth_session;
        return v;
    }

    static AuditLog fromJson(const Json::Value &v) {
        AuditLog log;
        log.id = v["id"].asInt();
        log.created_time_sec = v["createdTimeSec"].asUInt64();
        log.range_start_sec = v["rangeStartSec"].asUInt64();
        log.range_end_sec = v["rangeEndSec"].asUInt64();
        log.event_type = v["eventType"].asInt();
        log.resources = v["resources"].asString();
        log.params = v["params"].asString();
        log.auth_session = v["authSession"].asString();
        return log;
    }
};

DECLARE_ENTITY(AuditLog, "audit_log", 
    {"id"},
    &AuditLog::id, "id",
    &AuditLog::created_time_sec, "createdTimeSec",
    &AuditLog::range_start_sec, "rangeStartSec",
    &AuditLog::range_end_sec, "rangeEndSec",
    &AuditLog::event_type, "eventType",
    &AuditLog::resources, "resources",
    &AuditLog::params, "params",
    &AuditLog::auth_session, "authSession"
)

class AuditLogRepository : public SqliteRepository<AuditLog> {
public:
    AuditLogRepository() : SqliteRepository<AuditLog>(Database::kMediaServerDb) {}

    std::vector<AuditLog> findByTimeRange(uint64_t start_sec, uint64_t end_sec, int event_type, const std::vector<std::string> &resource_guids, int offset, int size, const std::string &sort = "DESC") {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "created_time_sec > ?";
        whereParams.push_back(std::to_string(start_sec));

        if (end_sec > 0) {
            whereClause << " AND created_time_sec < ?";
            whereParams.push_back(std::to_string(end_sec));
        }

        if (event_type >= 0) {
            whereClause << " AND event_type = ?";
            whereParams.push_back(std::to_string(event_type));
        }

        if (!resource_guids.empty()) {
            whereClause << " AND resources IN (";
            for (size_t i = 0; i < resource_guids.size(); ++i) {
                if (i > 0) {
                    whereClause << ", ";
                }
                whereClause << "?";
                whereParams.push_back(resource_guids[i]);
            }
            whereClause << ")";
        }

        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<AuditLog>::getColumns())
                         .from(EntityTraits<AuditLog>::tableName())
                         .where(whereClause.str(), whereParams)
                         .orderBy("createdTimeSec " + sort)
                         .limit(size)
                         .offset(offset);
        auto rows = _executor->executeRaw(query);
        std::vector<AuditLog> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<AuditLog>::fromRow(row));
        }
        return ret;
    }

    int countByTimeRange(uint64_t start_sec, uint64_t end_sec, const std::vector<std::string> &resource_guids) {
        std::ostringstream whereClause;
        std::vector<std::string> whereParams;

        whereClause << "created_time_sec > ?";
        whereParams.push_back(std::to_string(start_sec));

        if (end_sec > 0) {
            whereClause << " AND created_time_sec < ?";
            whereParams.push_back(std::to_string(end_sec));
        }

        if (!resource_guids.empty()) {
            whereClause << " AND resources IN (";
            for (size_t i = 0; i < resource_guids.size(); ++i) {
                if (i > 0) {
                    whereClause << ", ";
                }
                whereClause << "?";
                whereParams.push_back(resource_guids[i]);
            }
            whereClause << ")";
        }

        auto query = toolkit::QueryBuilder()
                         .select({"COUNT(*)"})
                         .from(EntityTraits<AuditLog>::tableName())
                         .where(whereClause.str(), whereParams);
        auto rows = _executor->executeRaw(query);
        if (!rows.empty() && !rows[0].empty()) {
            return std::stoi(rows[0][0]);
        }
        return 0;
    }
};

class AuditLogImp : public AuditLogRepository {
public:
    using Ptr = std::shared_ptr<AuditLogImp>;
    AuditLogImp() : AuditLogRepository() {};

    void add(AuditLog &log) {
        save(log);
    }

    std::vector<AuditLog> search(int64_t start_sec, uint64_t end_sec, int event_type, const std::string &resources, int page, int size, std::string sort) {
        std::vector<std::string> _resource_guids;
        if (!resources.empty()) {
            _resource_guids = toolkit::split(resources, ",");
        }
        std::string _sort = toolkit::strToLower(sort) == "desc" ? "DESC" : "ASC";
        int offset = page * size;
        return findByTimeRange(start_sec, end_sec, event_type, _resource_guids, offset, size, _sort);
    }

    int count(int64_t start_sec, int64_t end_sec, const std::string &resources) {
        std::vector<std::string> _resource_guids;
        if (!resources.empty()) {
            _resource_guids = toolkit::split(resources, ",");
        }
        return countByTimeRange(start_sec, end_sec, _resource_guids);
    }
};

} // namespace managerkit

#endif // STORAGE_AUDIT_LOG_H