#ifndef S3MANAGERKIT_EXTRACT_JOB_H
#define S3MANAGERKIT_EXTRACT_JOB_H

#include <ctime>
#include <string>
#include <vector>
#include <json/json.h>

#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/QueryBuilder.h"

namespace managerkit {

enum class ExtractJobStatus {
    PENDING,
    EXTRACTING,
    UPLOADING,
    SUCCESS,
    FAILED
};

enum class ExtractCallbackStatus {
    NONE,
    PENDING,
    SENDING,
    DELIVERED,
    DEAD
};

inline const char *extractJobStatusToString(ExtractJobStatus status) {
    switch (status) {
        case ExtractJobStatus::PENDING: return "PENDING";
        case ExtractJobStatus::EXTRACTING: return "EXTRACTING";
        case ExtractJobStatus::UPLOADING: return "UPLOADING";
        case ExtractJobStatus::SUCCESS: return "SUCCESS";
        case ExtractJobStatus::FAILED: return "FAILED";
    }
    return "FAILED";
}

inline const char *extractCallbackStatusToString(ExtractCallbackStatus status) {
    switch (status) {
        case ExtractCallbackStatus::NONE: return "NONE";
        case ExtractCallbackStatus::PENDING: return "PENDING";
        case ExtractCallbackStatus::SENDING: return "SENDING";
        case ExtractCallbackStatus::DELIVERED: return "DELIVERED";
        case ExtractCallbackStatus::DEAD: return "DEAD";
    }
    return "DEAD";
}

struct ExtractJob {
    std::string file_id;
    std::string request_hash;
    std::string camera_id;
    std::string stream_id;
    int64_t start_time = 0;
    int64_t end_time = 0;
    std::string format;
    std::string upload_url;
    std::string status = "PENDING";
    float progress_percent = 0.0f;
    std::string local_path;
    int extract_attempts = 0;
    int upload_attempts = 0;
    int64_t next_attempt_at = 0;
    int64_t size_bytes = 0;
    int64_t duration_seconds = 0;
    std::string content_type;
    std::string error_code;
    std::string error_message;
    std::string callback_status = "NONE";
    int callback_attempts = 0;
    int64_t next_callback_at = 0;
    std::string callback_last_error;
    int64_t notified_at = 0;
    int64_t completed_at = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;

    Json::Value toJson() const {
        Json::Value value;
        value["fileId"] = file_id;
        value["cameraId"] = camera_id;
        value["streamId"] = stream_id;
        value["startTime"] = static_cast<Json::Int64>(start_time);
        value["endTime"] = static_cast<Json::Int64>(end_time);
        value["format"] = format;
        value["status"] = status;
        value["progressPercent"] = progress_percent;
        value["callbackStatus"] = callback_status;
        value["completedAt"] = static_cast<Json::Int64>(completed_at);
        return value;
    }

    Json::Value toCallbackJson() const {
        Json::Value value;
        value["fileId"] = file_id;
        value["status"] = status;
        value["completedAt"] = static_cast<Json::Int64>(completed_at);
        if (status == extractJobStatusToString(ExtractJobStatus::SUCCESS)) {
            value["file"]["sizeBytes"] = static_cast<Json::Int64>(size_bytes);
            value["file"]["durationSeconds"] = static_cast<Json::Int64>(duration_seconds);
            value["file"]["contentType"] = content_type;
        } else {
            value["error"]["code"] = error_code;
            value["error"]["message"] = error_message;
        }
        return value;
    }
};

DECLARE_ENTITY(ExtractJob, "extract_jobs",
    {"file_id"},
    &ExtractJob::file_id, "file_id",
    &ExtractJob::request_hash, "request_hash",
    &ExtractJob::camera_id, "camera_id",
    &ExtractJob::stream_id, "stream_id",
    &ExtractJob::start_time, "start_time",
    &ExtractJob::end_time, "end_time",
    &ExtractJob::format, "format",
    &ExtractJob::upload_url, "upload_url",
    &ExtractJob::status, "status",
    &ExtractJob::progress_percent, "progress_percent",
    &ExtractJob::local_path, "local_path",
    &ExtractJob::extract_attempts, "extract_attempts",
    &ExtractJob::upload_attempts, "upload_attempts",
    &ExtractJob::next_attempt_at, "next_attempt_at",
    &ExtractJob::size_bytes, "size_bytes",
    &ExtractJob::duration_seconds, "duration_seconds",
    &ExtractJob::content_type, "content_type",
    &ExtractJob::error_code, "error_code",
    &ExtractJob::error_message, "error_message",
    &ExtractJob::callback_status, "callback_status",
    &ExtractJob::callback_attempts, "callback_attempts",
    &ExtractJob::next_callback_at, "next_callback_at",
    &ExtractJob::callback_last_error, "callback_last_error",
    &ExtractJob::notified_at, "notified_at",
    &ExtractJob::completed_at, "completed_at",
    &ExtractJob::created_at, "created_at",
    &ExtractJob::updated_at, "updated_at"
)

class ExtractJobRepository : public SqliteRepository<ExtractJob> {
public:
    enum class CreateResult {
        CREATED,
        EXISTING,
        CONFLICT,
        FAILED
    };

    ExtractJobRepository() : SqliteRepository<ExtractJob>(Database::kMediaServerDb) {}

    std::vector<ExtractJob> findByFileId(const std::string &file_id) {
        ExtractJob job;
        job.file_id = file_id;
        return SqliteRepository<ExtractJob>::findById(job);
    }

    CreateResult createOrFind(const ExtractJob &job, ExtractJob &stored) {
        try {
            auto txn = _executor->execTxn();
            ExtractJob lookup;
            lookup.file_id = job.file_id;
            auto rows = findByIdWithTxn(lookup, txn);
            if (!rows.empty()) {
                stored = rows.front();
                txn->commit();
                return stored.request_hash == job.request_hash ? CreateResult::EXISTING : CreateResult::CONFLICT;
            }
            if (!saveWithTxn(job, txn, true)) {
                return CreateResult::FAILED;
            }
            txn->commit();
            stored = job;
            return CreateResult::CREATED;
        } catch (const std::exception &) {
            // A concurrent insert may win between transactions. Resolve it as
            // idempotent instead of exposing a database race to the caller.
            auto rows = findByFileId(job.file_id);
            if (rows.empty()) {
                return CreateResult::FAILED;
            }
            stored = rows.front();
            return stored.request_hash == job.request_hash ? CreateResult::EXISTING : CreateResult::CONFLICT;
        }
    }

    bool claimExtract(const std::string &file_id, int extract_attempts, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"status", "EXTRACTING"},
                      {"extract_attempts", std::to_string(extract_attempts + 1)},
                      {"next_attempt_at", "0"},
                      {"updated_at", std::to_string(now)}})
                .where("file_id = ? AND status = ?", {file_id, "PENDING"}));
    }

    bool markUploading(const std::string &file_id, const std::string &local_path,
                       int64_t size_bytes, int64_t duration_seconds,
                       const std::string &content_type, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"status", "UPLOADING"}, {"progress_percent", "100"},
                      {"local_path", local_path}, {"size_bytes", std::to_string(size_bytes)},
                      {"duration_seconds", std::to_string(duration_seconds)},
                      {"content_type", content_type}, {"next_attempt_at", "0"},
                      {"updated_at", std::to_string(now)}})
                .where("file_id = ? AND status = ?", {file_id, "EXTRACTING"}));
    }

    bool claimUpload(const std::string &file_id, int upload_attempts, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"upload_attempts", std::to_string(upload_attempts + 1)},
                      {"next_attempt_at", "0"}, {"updated_at", std::to_string(now)}})
                .where("file_id = ? AND status = ? AND (next_attempt_at = 0 OR next_attempt_at <= ?)",
                       {file_id, "UPLOADING", std::to_string(now)}));
    }

    bool scheduleUploadRetry(const std::string &file_id, int64_t next_attempt_at,
                             const std::string &error, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"next_attempt_at", std::to_string(next_attempt_at)},
                      {"error_message", error}, {"updated_at", std::to_string(now)}})
                .where("file_id = ? AND status = ?", {file_id, "UPLOADING"}));
    }

    bool markSuccess(const std::string &file_id, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"status", "SUCCESS"}, {"callback_status", "PENDING"},
                      {"next_callback_at", std::to_string(now)},
                      {"completed_at", std::to_string(now)},
                      {"updated_at", std::to_string(now)}, {"upload_url", ""}})
                .where("file_id = ? AND status = ?", {file_id, "UPLOADING"}));
    }

    bool markFailed(const std::string &file_id, const std::string &error_code,
                    const std::string &error_message, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"status", "FAILED"}, {"error_code", error_code},
                      {"error_message", error_message}, {"callback_status", "PENDING"},
                      {"next_callback_at", std::to_string(now)},
                      {"completed_at", std::to_string(now)},
                      {"updated_at", std::to_string(now)}, {"upload_url", ""}})
                .where("file_id = ? AND status NOT IN (?, ?)", {file_id, "SUCCESS", "FAILED"}));
    }

    bool claimCallback(const std::string &file_id, int callback_attempts, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"callback_status", "SENDING"},
                      {"callback_attempts", std::to_string(callback_attempts + 1)},
                      {"updated_at", std::to_string(now)}})
                .where("file_id = ? AND callback_status = ? AND (next_callback_at = 0 OR next_callback_at <= ?)",
                       {file_id, "PENDING", std::to_string(now)}));
    }

    bool scheduleCallbackRetry(const std::string &file_id, int64_t next_callback_at,
                               const std::string &error, bool dead, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"callback_status", dead ? "DEAD" : "PENDING"},
                      {"next_callback_at", std::to_string(next_callback_at)},
                      {"callback_last_error", error}, {"updated_at", std::to_string(now)}})
                .where("file_id = ? AND callback_status = ?", {file_id, "SENDING"}));
    }

    bool markCallbackDelivered(const std::string &file_id, int64_t now) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"callback_status", "DELIVERED"}, {"notified_at", std::to_string(now)},
                      {"callback_last_error", ""}, {"updated_at", std::to_string(now)}})
                .where("file_id = ? AND callback_status = ?", {file_id, "SENDING"}));
    }

    std::vector<ExtractJob> findDueProcessing(int64_t now, int limit) {
        return queryWhere(
            "(status='PENDING' OR status='UPLOADING') AND (next_attempt_at=0 OR next_attempt_at<=?)",
            {std::to_string(now)}, "created_at ASC", limit);
    }

    std::vector<ExtractJob> findDueCallbacks(int64_t now, int limit) {
        return queryWhere(
            "callback_status='PENDING' AND (next_callback_at=0 OR next_callback_at<=?)",
            {std::to_string(now)}, "completed_at ASC", limit);
    }

    void recoverInterrupted(int64_t now) {
        _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"status", "PENDING"}, {"next_attempt_at", "0"},
                      {"updated_at", std::to_string(now)}})
                .where("status = ?", {"EXTRACTING"}));
        _executor->execDML(
            toolkit::QueryBuilder()
                .update(EntityTraits<ExtractJob>::tableName())
                .set({{"callback_status", "PENDING"}, {"next_callback_at", "0"},
                      {"updated_at", std::to_string(now)}})
                .where("callback_status = ?", {"SENDING"}));
    }

private:
    std::vector<ExtractJob> queryWhere(const std::string &where,
                                       const std::vector<std::string> &params,
                                       const std::string &order,
                                       int limit) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<ExtractJob>::getColumns())
                .from(EntityTraits<ExtractJob>::tableName())
                .where(where, params)
                .orderBy(order)
                .limit(limit));
        std::vector<ExtractJob> result;
        for (const auto &row : rows) {
            result.push_back(EntityTraits<ExtractJob>::fromRow(row));
        }
        return result;
    }
};

class ExtractJobImp : public ExtractJobRepository {
public:
    using Ptr = std::shared_ptr<ExtractJobImp>;
};

} // namespace managerkit

#endif // S3MANAGERKIT_EXTRACT_JOB_H
