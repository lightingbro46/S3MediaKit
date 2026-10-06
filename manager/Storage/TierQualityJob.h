#ifndef STORAGE_TIERQUALITYJOB_H
#define STORAGE_TIERQUALITYJOB_H

#ifdef ENABLE_TIER_STORAGE

#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "DbSchema.h"
#include "DbStorage.h"
#include "TieringJob.h"

namespace managerkit {

struct TieringJobSegment {
    std::string job_id;
    std::string range_id;
    std::string camera_id;
    std::string stream_id;
    std::string segment_path;
    std::string storage_key;
    int64_t segment_start_time = 0;
    int64_t segment_end_time = 0;
    std::string operation = "COPY_MOVE";
    std::string profile_fingerprint;
    std::string encoder_backend;
    std::string status = "PENDING";
    int attempt_count = 0;
    int64_t source_bytes = 0;
    int64_t output_bytes = 0;
    std::string error_code;
    std::string error_message;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

DECLARE_ENTITY(TieringJobSegment, "tiering_job_segments",
    MAKE_PK("job_id", "stream_id", "segment_path"),
    &TieringJobSegment::job_id, "job_id",
    &TieringJobSegment::range_id, "range_id",
    &TieringJobSegment::camera_id, "camera_id",
    &TieringJobSegment::stream_id, "stream_id",
    &TieringJobSegment::segment_path, "segment_path",
    &TieringJobSegment::storage_key, "storage_key",
    &TieringJobSegment::segment_start_time, "segment_start_time",
    &TieringJobSegment::segment_end_time, "segment_end_time",
    &TieringJobSegment::operation, "operation",
    &TieringJobSegment::profile_fingerprint, "profile_fingerprint",
    &TieringJobSegment::encoder_backend, "encoder_backend",
    &TieringJobSegment::status, "status",
    &TieringJobSegment::attempt_count, "attempt_count",
    &TieringJobSegment::source_bytes, "source_bytes",
    &TieringJobSegment::output_bytes, "output_bytes",
    &TieringJobSegment::error_code, "error_code",
    &TieringJobSegment::error_message, "error_message",
    &TieringJobSegment::created_at, "created_at",
    &TieringJobSegment::updated_at, "updated_at"
)

struct TierTimefileSizeUpdate {
    std::string job_id;
    std::string range_id;
    std::string camera_id;
    std::string stream_id;
    std::string segment_path;
    int64_t start_time = 0;
    int64_t old_size = 0;
    int64_t new_size = 0;
    std::string status = "PENDING";
    int attempt_count = 0;
    std::string error_message;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

DECLARE_ENTITY(TierTimefileSizeUpdate, "tier_timefile_size_updates",
    MAKE_PK("camera_id", "stream_id", "segment_path"),
    &TierTimefileSizeUpdate::job_id, "job_id",
    &TierTimefileSizeUpdate::range_id, "range_id",
    &TierTimefileSizeUpdate::camera_id, "camera_id",
    &TierTimefileSizeUpdate::stream_id, "stream_id",
    &TierTimefileSizeUpdate::segment_path, "segment_path",
    &TierTimefileSizeUpdate::start_time, "start_time",
    &TierTimefileSizeUpdate::old_size, "old_size",
    &TierTimefileSizeUpdate::new_size, "new_size",
    &TierTimefileSizeUpdate::status, "status",
    &TierTimefileSizeUpdate::attempt_count, "attempt_count",
    &TierTimefileSizeUpdate::error_message, "error_message",
    &TierTimefileSizeUpdate::created_at, "created_at",
    &TierTimefileSizeUpdate::updated_at, "updated_at"
)

class TieringJobSegmentRepository : public SqliteRepository<TieringJobSegment> {
public:
    TieringJobSegmentRepository()
        : SqliteRepository<TieringJobSegment>(Database::kMediaServerDb) {}

    bool insertIfAbsent(const TieringJobSegment &segment) { return save(segment, true); }

    std::vector<TieringJobSegment> findByJob(const std::string &job_id) {
        std::vector<TieringJobSegment> result;
        auto rows = _executor->executeRaw(toolkit::QueryBuilder()
            .select(EntityTraits<TieringJobSegment>::getColumns())
            .from(EntityTraits<TieringJobSegment>::tableName())
            .where("job_id = ?", {job_id})
            .orderBy("segment_start_time ASC"));
        for (const auto &row : rows)
            result.push_back(EntityTraits<TieringJobSegment>::fromRow(row));
        return result;
    }

    bool updateResultAt(const TieringJobSegment &segment, const std::string &status,
                        int64_t output_bytes, const std::string &encoder,
                        const std::string &error_code, const std::string &error_message,
                        int64_t updated_at) {
        return _executor->execDML(toolkit::QueryBuilder()
            .update(EntityTraits<TieringJobSegment>::tableName())
            .set({{"status", status},
                  {"attempt_count", std::to_string(segment.attempt_count + 1)},
                  {"output_bytes", std::to_string(output_bytes)},
                  {"encoder_backend", encoder},
                  {"error_code", error_code},
                  {"error_message", error_message},
                  {"updated_at", std::to_string(updated_at)}})
            .where("job_id = ? AND stream_id = ? AND segment_path = ?",
                   {segment.job_id, segment.stream_id, segment.segment_path}));
    }

    void recoverTransformingAt(int64_t updated_at) {
        _executor->execDML(toolkit::QueryBuilder()
            .update(EntityTraits<TieringJobSegment>::tableName())
            .set({{"status", "PENDING"},
                  {"error_code", "TIER_TRANSCODE_RECOVERED"},
                  {"error_message", "Recovered after restart"},
                  {"updated_at", std::to_string(updated_at)}})
            .where("status = ?", {"TRANSFORMING"}));
    }

    void resetFailedForRetryAt(const std::string &job_id, int64_t updated_at) {
        _executor->execDML(toolkit::QueryBuilder()
            .update(EntityTraits<TieringJobSegment>::tableName())
            .set({{"status", "PENDING"}, {"error_code", ""}, {"error_message", ""},
                  {"updated_at", std::to_string(updated_at)}})
            .where("job_id = ? AND status = ?", {job_id, "FAILED"}));
    }

    std::vector<TieringJobSegment> findCommittedForCleanup(int limit = 1000) {
        std::vector<TieringJobSegment> result;
        auto rows = _executor->executeRaw(toolkit::QueryBuilder()
            .select(EntityTraits<TieringJobSegment>::getColumns())
            .from(EntityTraits<TieringJobSegment>::tableName())
            .where("status = ?", {"COMMITTED"})
            .orderBy("updated_at ASC")
            .limit(limit));
        for (const auto &row : rows)
            result.push_back(EntityTraits<TieringJobSegment>::fromRow(row));
        return result;
    }

    bool markCleanedAt(const TieringJobSegment &segment, int64_t updated_at) {
        return _executor->execDML(toolkit::QueryBuilder()
            .update(EntityTraits<TieringJobSegment>::tableName())
            .set({{"status", "CLEANED"}, {"updated_at", std::to_string(updated_at)}})
            .where("job_id = ? AND stream_id = ? AND segment_path = ? AND status = ?",
                   {segment.job_id, segment.stream_id, segment.segment_path, "COMMITTED"}));
    }
};

class TieringJobSegmentImp : public TieringJobSegmentRepository {
public:
    using Ptr = std::shared_ptr<TieringJobSegmentImp>;
    bool add(TieringJobSegment segment) {
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (segment.created_at <= 0) segment.created_at = now;
        segment.updated_at = now;
        return insertIfAbsent(segment);
    }

    bool updateResult(const TieringJobSegment &segment, const std::string &status,
                      int64_t output_bytes, const std::string &encoder,
                      const std::string &error_code = "",
                      const std::string &error_message = "") {
        return updateResultAt(segment, status, output_bytes, encoder, error_code,
                              error_message, static_cast<int64_t>(time(nullptr)));
    }
    void recoverTransforming() {
        recoverTransformingAt(static_cast<int64_t>(time(nullptr)));
    }
    void resetFailedForRetry(const std::string &job_id) {
        resetFailedForRetryAt(job_id, static_cast<int64_t>(time(nullptr)));
    }
    bool markCleaned(const TieringJobSegment &segment) {
        return markCleanedAt(segment, static_cast<int64_t>(time(nullptr)));
    }
};

class TierTimefileSizeUpdateRepository : public SqliteRepository<TierTimefileSizeUpdate> {
public:
    TierTimefileSizeUpdateRepository()
        : SqliteRepository<TierTimefileSizeUpdate>(Database::kMediaServerDb) {}

    bool upsert(const TierTimefileSizeUpdate &update) {
        auto txn = _executor->beginWriteTxn();
        try {
            _executor->execDMLWithTxn(txn, toolkit::QueryBuilder()
                .deleteFrom(EntityTraits<TierTimefileSizeUpdate>::tableName())
                .where("camera_id = ? AND stream_id = ? AND segment_path = ?",
                       {update.camera_id, update.stream_id, update.segment_path}));
            if (!saveWithTxn(update, txn, true)) {
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

    std::vector<TierTimefileSizeUpdate> findPending(int limit = 1000) {
        std::vector<TierTimefileSizeUpdate> result;
        auto rows = _executor->executeRaw(toolkit::QueryBuilder()
            .select(EntityTraits<TierTimefileSizeUpdate>::getColumns())
            .from(EntityTraits<TierTimefileSizeUpdate>::tableName())
            .where("status = ?", {"PENDING"})
            .orderBy("updated_at ASC")
            .limit(limit));
        for (const auto &row : rows)
            result.push_back(EntityTraits<TierTimefileSizeUpdate>::fromRow(row));
        return result;
    }

    std::vector<TierTimefileSizeUpdate> findPendingByRangeId(
            const std::string &range_id) {
        std::vector<TierTimefileSizeUpdate> result;
        if (range_id.empty()) return result;
        auto rows = _executor->executeRaw(toolkit::QueryBuilder()
            .select(EntityTraits<TierTimefileSizeUpdate>::getColumns())
            .from(EntityTraits<TierTimefileSizeUpdate>::tableName())
            .where("range_id = ? AND status = ?", {range_id, "PENDING"}));
        for (const auto &row : rows)
            result.push_back(EntityTraits<TierTimefileSizeUpdate>::fromRow(row));
        return result;
    }

    bool markResultAt(const TierTimefileSizeUpdate &update, const std::string &status,
                      const std::string &error, int64_t updated_at) {
        return _executor->execDML(toolkit::QueryBuilder()
            .update(EntityTraits<TierTimefileSizeUpdate>::tableName())
            .set({{"status", status},
                  {"attempt_count", std::to_string(update.attempt_count + 1)},
                  {"error_message", error},
                  {"updated_at", std::to_string(updated_at)}})
            .where("camera_id = ? AND stream_id = ? AND segment_path = ? AND job_id = ?",
                   {update.camera_id, update.stream_id, update.segment_path, update.job_id}));
    }
};

class TierTimefileSizeUpdateImp : public TierTimefileSizeUpdateRepository {
public:
    using Ptr = std::shared_ptr<TierTimefileSizeUpdateImp>;
    bool add(TierTimefileSizeUpdate update) {
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (update.created_at <= 0) update.created_at = now;
        update.updated_at = now;
        return upsert(update);
    }
    bool markResult(const TierTimefileSizeUpdate &update, const std::string &status,
                    const std::string &error = "") {
        return markResultAt(update, status, error, static_cast<int64_t>(time(nullptr)));
    }
};

// Commits the catalog-visible side of a quality-reducing transfer in one
// BEGIN IMMEDIATE transaction. Media targets are already durable at this
// point; a failed compare-and-set leaves them owned by STORED journal rows so
// retry/recovery can validate and reuse or clean them without deleting source.
class TierQualityTransferRepository : public SqliteRepository<TieringJobSegment> {
public:
    TierQualityTransferRepository()
        : SqliteRepository<TieringJobSegment>(Database::kMediaServerDb) {}

    bool finalizeTransfer(const TieringJob &job,
                          const SegmentTierRange &expected_range,
                          const std::string &target_tier,
                          const std::string &target_pool_id,
                          const std::string &range_status,
                          int64_t kept_count,
                          int64_t kept_size,
                          const std::vector<TierTimefileSizeUpdate> &outbox,
                          int64_t updated_at) {
        auto txn = _executor->beginWriteTxn();
        try {
            auto stored_rows = _executor->executeRawWithTxn(
                txn, toolkit::QueryBuilder()
                    .select({"COUNT(*)"})
                    .from(EntityTraits<TieringJobSegment>::tableName())
                    .where("job_id = ? AND status = ?", {job.job_id, "STORED"}));
            const int64_t stored_count = stored_rows.empty() || stored_rows[0].empty()
                ? 0 : std::strtoll(stored_rows[0][0].c_str(), nullptr, 10);
            if (stored_count != kept_count) {
                txn->rollback();
                return false;
            }

            std::vector<std::pair<std::string, std::string>> range_values;
            range_values.push_back({"tier", target_tier});
            range_values.push_back({"pool_id", target_pool_id});
            range_values.push_back({"status", range_status});
            range_values.push_back({"segment_count", std::to_string(kept_count)});
            range_values.push_back({"size_bytes", std::to_string(kept_size)});
            range_values.push_back({"updated_at", std::to_string(updated_at)});
            if (!_executor->execDMLWithTxn(
                    txn, toolkit::QueryBuilder()
                        .update(EntityTraits<SegmentTierRange>::tableName())
                        .set(range_values)
                        .where(SegmentTierRangeRepository::snapshotWhereClause(),
                               SegmentTierRangeRepository::snapshotWhereParams(expected_range)))) {
                txn->rollback();
                return false;
            }

            if (kept_count > 0 && !_executor->execDMLWithTxn(
                    txn, toolkit::QueryBuilder()
                        .update(EntityTraits<TieringJobSegment>::tableName())
                        .set({{"status", "COMMITTED"},
                              {"updated_at", std::to_string(updated_at)}})
                        .where("job_id = ? AND status = ?", {job.job_id, "STORED"}))) {
                txn->rollback();
                return false;
            }

            for (const auto &update : outbox) {
                _executor->execDMLWithTxn(
                    txn, toolkit::QueryBuilder()
                        .deleteFrom(EntityTraits<TierTimefileSizeUpdate>::tableName())
                        .where("camera_id = ? AND stream_id = ? AND segment_path = ?",
                               {update.camera_id, update.stream_id, update.segment_path}));
                const std::vector<std::string> columns =
                    EntityTraits<TierTimefileSizeUpdate>::getColumns();
                const std::vector<std::string> values =
                    EntityTraits<TierTimefileSizeUpdate>::getValues(update);
                std::vector<std::pair<std::string, std::string>> assignments;
                for (size_t i = 0; i < columns.size(); ++i)
                    assignments.push_back(std::make_pair(columns[i], values[i]));
                if (!_executor->execDMLWithTxn(
                        txn, toolkit::QueryBuilder()
                            .insertInto(EntityTraits<TierTimefileSizeUpdate>::tableName())
                            .values(assignments))) {
                    txn->rollback();
                    return false;
                }
            }

            if (!_executor->execDMLWithTxn(
                    txn, toolkit::QueryBuilder()
                        .update(EntityTraits<TieringJob>::tableName())
                        .set({{"status", jobStatusToString(JobStatus::DONE)},
                              {"bytes_moved", std::to_string(job.bytes_moved)},
                              {"output_bytes", std::to_string(job.output_bytes)},
                              {"transcoded_segment_count", std::to_string(job.transcoded_segment_count)},
                              {"preserved_segment_count", std::to_string(job.preserved_segment_count)},
                              {"encoder_summary_json", job.encoder_summary_json},
                              {"error_message", ""},
                              {"updated_at", std::to_string(updated_at)}})
                        .where("job_id = ? AND status = ?",
                               {job.job_id, jobStatusToString(JobStatus::RUNNING)}))) {
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
};

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
#endif // STORAGE_TIERQUALITYJOB_H
