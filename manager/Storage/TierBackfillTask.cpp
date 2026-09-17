#include "TierBackfillTask.h"

#ifdef ENABLE_TIER_STORAGE

#include <algorithm>
#include <ctime>
#include "Common/StrUtil.h"
#include "Util/QueryBuilder.h"

using namespace toolkit;

namespace managerkit {

const char *tierBackfillTaskStatusToString(TierBackfillTaskStatus status) {
    switch (status) {
        case TierBackfillTaskStatus::PENDING: return "PENDING";
        case TierBackfillTaskStatus::RUNNING: return "RUNNING";
        case TierBackfillTaskStatus::RETRY: return "RETRY";
        case TierBackfillTaskStatus::COMPLETED: return "COMPLETED";
        case TierBackfillTaskStatus::FAILED: return "FAILED";
    }
    return "PENDING";
}

Json::Value TierBackfillTask::toJson() const {
    Json::Value value;
    value["task_id"] = task_id;
    value["camera_id"] = camera_id;
    value["timeline_path"] = timeline_path;
    value["window_start"] = static_cast<Json::Int64>(window_start);
    value["cutoff_time"] = static_cast<Json::Int64>(cutoff_time);
    value["cursor_time"] = static_cast<Json::Int64>(cursor_time);
    value["status"] = status;
    value["attempt_count"] = attempt_count;
    value["next_retry_at"] = static_cast<Json::Int64>(next_retry_at);
    value["last_error"] = last_error;
    value["scanned_blocks"] = static_cast<Json::Int64>(scanned_blocks);
    value["registered_blocks"] = static_cast<Json::Int64>(registered_blocks);
    value["orphan_blocks"] = static_cast<Json::Int64>(orphan_blocks);
    value["indexed_bytes"] = static_cast<Json::Int64>(indexed_bytes);
    value["started_at"] = static_cast<Json::Int64>(started_at);
    value["completed_at"] = static_cast<Json::Int64>(completed_at);
    value["created_at"] = static_cast<Json::Int64>(created_at);
    value["updated_at"] = static_cast<Json::Int64>(updated_at);
    return value;
}

TierBackfillTaskRepository::TierBackfillTaskRepository()
    : SqliteRepository<TierBackfillTask>(Database::kMediaServerDb) {}

std::vector<TierBackfillTask> TierBackfillTaskRepository::findByTaskId(const std::string &task_id) {
    TierBackfillTask task;
    task.task_id = task_id;
    return SqliteRepository<TierBackfillTask>::findById(task);
}

std::vector<TierBackfillTask> TierBackfillTaskRepository::findByCameraId(const std::string &camera_id) {
    auto rows = _executor->executeRaw(
        QueryBuilder()
            .select(EntityTraits<TierBackfillTask>::getColumns())
            .from(EntityTraits<TierBackfillTask>::tableName())
            .where("camera_id = ?", {camera_id})
            .limit(1));
    std::vector<TierBackfillTask> result;
    for (const auto &row : rows)
        result.push_back(EntityTraits<TierBackfillTask>::fromRow(row));
    return result;
}

std::vector<TierBackfillTask> TierBackfillTaskRepository::findAll() {
    auto rows = _executor->executeRaw(
        QueryBuilder()
            .select(EntityTraits<TierBackfillTask>::getColumns())
            .from(EntityTraits<TierBackfillTask>::tableName())
            .orderBy("updated_at ASC"));
    std::vector<TierBackfillTask> result;
    for (const auto &row : rows)
        result.push_back(EntityTraits<TierBackfillTask>::fromRow(row));
    return result;
}

bool TierBackfillTaskRepository::insertTask(const TierBackfillTask &task) {
    return save(task, true);
}

bool TierBackfillTaskRepository::updateDefinitionAt(const TierBackfillTask &task) {
    return _executor->execDML(
        QueryBuilder()
            .update(EntityTraits<TierBackfillTask>::tableName())
            .set({{"timeline_path", task.timeline_path},
                  {"window_start", std::to_string(task.window_start)},
                  {"cutoff_time", std::to_string(task.cutoff_time)},
                  {"cursor_time", std::to_string(task.cursor_time)},
                  {"status", task.status},
                  {"attempt_count", std::to_string(task.attempt_count)},
                  {"next_retry_at", std::to_string(task.next_retry_at)},
                  {"last_error", task.last_error},
                  {"completed_at", std::to_string(task.completed_at)},
                  {"updated_at", std::to_string(task.updated_at)}})
            .where("task_id = ? AND status <> ?",
                   {task.task_id, tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING)}));
}

bool TierBackfillTaskRepository::updateWindowAt(const TierBackfillTask &task) {
    return _executor->execDML(
        QueryBuilder()
            .update(EntityTraits<TierBackfillTask>::tableName())
            .set({{"window_start", std::to_string(task.window_start)},
                  {"cutoff_time", std::to_string(task.cutoff_time)},
                  {"cursor_time", std::to_string(task.cursor_time)},
                  {"status", task.status},
                  {"next_retry_at", std::to_string(task.next_retry_at)},
                  {"completed_at", std::to_string(task.completed_at)},
                  {"updated_at", std::to_string(task.updated_at)}})
            .where("task_id = ? AND status <> ?",
                   {task.task_id, tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING)}));
}

bool TierBackfillTaskRepository::retryFailedAt(const std::string &camera_id,
                                                int64_t updated_at) {
    return _executor->execDML(
        QueryBuilder()
            .update(EntityTraits<TierBackfillTask>::tableName())
            .set({{"status", tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING)},
                  {"attempt_count", "0"},
                  {"next_retry_at", "0"},
                  {"last_error", ""},
                  {"completed_at", "0"},
                  {"updated_at", std::to_string(updated_at)}})
            .where("camera_id = ? AND status = ?",
                   {camera_id, tierBackfillTaskStatusToString(TierBackfillTaskStatus::FAILED)}));
}

bool TierBackfillTaskRepository::hasDueAt(int64_t now) {
    auto rows = _executor->executeRaw(
        QueryBuilder()
            .select({"1"})
            .from(EntityTraits<TierBackfillTask>::tableName())
            .where("status IN (?, ?) AND next_retry_at <= ?",
                   {tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING),
                    tierBackfillTaskStatusToString(TierBackfillTaskStatus::RETRY),
                    std::to_string(now)})
            .limit(1));
    return !rows.empty();
}

std::vector<TierBackfillTask> TierBackfillTaskRepository::claimNextDueAt(int64_t now) {
    std::vector<TierBackfillTask> claimed;
    auto txn = _executor->execTxn();
    try {
        auto rows = _executor->executeRawWithTxn(
            txn,
            QueryBuilder()
                .select(EntityTraits<TierBackfillTask>::getColumns())
                .from(EntityTraits<TierBackfillTask>::tableName())
                .where("status IN (?, ?) AND next_retry_at <= ?",
                       {tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING),
                        tierBackfillTaskStatusToString(TierBackfillTaskStatus::RETRY),
                        std::to_string(now)})
                .orderBy("updated_at ASC")
                .limit(1));
        if (!rows.empty()) {
            TierBackfillTask task = EntityTraits<TierBackfillTask>::fromRow(rows.front());
            int64_t started_at = task.started_at > 0 ? task.started_at : now;
            if (_executor->execDMLWithTxn(
                    txn,
                    QueryBuilder()
                        .update(EntityTraits<TierBackfillTask>::tableName())
                        .set({{"status", tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING)},
                              {"started_at", std::to_string(started_at)},
                              {"updated_at", std::to_string(now)}})
                        .where("task_id = ? AND status = ? AND cursor_time = ?",
                               {task.task_id, task.status, std::to_string(task.cursor_time)}))) {
                task.status = tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING);
                task.started_at = started_at;
                task.updated_at = now;
                claimed.push_back(task);
            }
        }
        txn->commit();
    } catch (...) {
        txn->rollback();
        throw;
    }
    return claimed;
}

void TierBackfillTaskRepository::recoverRunningAt(int64_t updated_at) {
    _executor->execDML(
        QueryBuilder()
            .update(EntityTraits<TierBackfillTask>::tableName())
            .set({{"status", tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING)},
                  {"next_retry_at", "0"},
                  {"last_error", "Recovered after restart"},
                  {"updated_at", std::to_string(updated_at)}})
            .where("status = ?",
                   {tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING)}));
}

bool TierBackfillTaskRepository::markFailureAt(const TierBackfillTask &task,
                                               const std::string &status,
                                               int attempt_count,
                                               int64_t next_retry_at,
                                               const std::string &error,
                                               int64_t scanned_blocks,
                                               int64_t orphan_blocks,
                                               int64_t updated_at) {
    return _executor->execDML(
        QueryBuilder()
            .update(EntityTraits<TierBackfillTask>::tableName())
            .set({{"status", status},
                  {"attempt_count", std::to_string(attempt_count)},
                  {"next_retry_at", std::to_string(next_retry_at)},
                  {"last_error", error},
                  {"scanned_blocks", std::to_string(task.scanned_blocks + scanned_blocks)},
                  {"orphan_blocks", std::to_string(task.orphan_blocks + orphan_blocks)},
                  {"updated_at", std::to_string(updated_at)}})
            .where("task_id = ? AND status = ? AND cursor_time = ?",
                   {task.task_id,
                    tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING),
                    std::to_string(task.cursor_time)}));
}

bool TierBackfillTaskRepository::commitChunkAt(const TierBackfillTask &task,
                                               const TierBackfillChunk &chunk,
                                               const std::string &status,
                                               int64_t completed_at,
                                               int64_t updated_at) {
    auto txn = _executor->execTxn();
    try {
        auto task_rows = _executor->executeRawWithTxn(
            txn,
            QueryBuilder()
                .select({"1"})
                .from(EntityTraits<TierBackfillTask>::tableName())
                .where("task_id = ? AND status = ? AND cursor_time = ?",
                       {task.task_id,
                        tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING),
                        std::to_string(task.cursor_time)})
                .limit(1));
        if (task_rows.empty()) {
            txn->rollback();
            return false;
        }

        SegmentTierRangeRepository range_repository;
        for (size_t index = 0; index < chunk.ranges.size(); ++index) {
            const auto &range = chunk.ranges[index];
            // A compact-only backfill must never overwrite data already moved
            // by tiering while historical indexing was running.
            // Pieces were planned around existing ranges. A conflict means
            // metadata changed meanwhile; retry the chunk so its blocks are
            // re-planned instead of silently skipped.
            if (range_repository.hasOwnershipConflictWithTxn(
                    txn, range.camera_id, range.stream_id,
                    range.start_time, range.end_time)) {
                txn->rollback();
                return false;
            }
            if (!range_repository.mergeNormalizedRangeWithTxn(
                    range, 90, updated_at, txn)) {
                txn->rollback();
                return false;
            }
        }

        bool updated = _executor->execDMLWithTxn(
            txn,
            QueryBuilder()
                .update(EntityTraits<TierBackfillTask>::tableName())
                .set({{"cursor_time", std::to_string(chunk.end_time)},
                      {"status", status},
                      {"attempt_count", "0"},
                      {"next_retry_at", status == tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING)
                                            ? std::to_string(updated_at + 1) : "0"},
                      {"last_error", ""},
                      {"scanned_blocks", std::to_string(task.scanned_blocks + chunk.scanned_blocks)},
                      {"registered_blocks", std::to_string(task.registered_blocks + chunk.registered_blocks)},
                      {"orphan_blocks", std::to_string(task.orphan_blocks + chunk.orphan_blocks)},
                      {"indexed_bytes", std::to_string(task.indexed_bytes + chunk.indexed_bytes)},
                      {"completed_at", std::to_string(completed_at)},
                      {"updated_at", std::to_string(updated_at)}})
                .where("task_id = ? AND status = ? AND cursor_time = ?",
                       {task.task_id,
                        tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING),
                        std::to_string(task.cursor_time)}));
        if (!updated) {
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

TierBackfillTask TierBackfillTaskImp::ensureTask(TierBackfillTask desired) {
    int64_t now = static_cast<int64_t>(time(nullptr));
    auto existing_rows = findByCameraId(desired.camera_id);
    if (existing_rows.empty()) {
        if (desired.task_id.empty())
            desired.task_id = StrUUID::make_guid(8, "bfl");
        desired.cursor_time = std::max(desired.window_start, desired.cursor_time);
        bool completed = desired.cursor_time >= desired.cutoff_time;
        desired.status = completed
            ? tierBackfillTaskStatusToString(TierBackfillTaskStatus::COMPLETED)
            : tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING);
        desired.attempt_count = 0;
        desired.next_retry_at = 0;
        desired.last_error.clear();
        desired.completed_at = completed ? now : 0;
        desired.created_at = now;
        desired.updated_at = now;
        if (!insertTask(desired))
            return TierBackfillTask();
        return desired;
    }

    TierBackfillTask existing = existing_rows.front();
    if (existing.status == tierBackfillTaskStatusToString(TierBackfillTaskStatus::RUNNING))
        return existing;

    bool path_changed = existing.timeline_path != desired.timeline_path;
    bool expanded = desired.window_start < existing.window_start;
    bool narrowed = desired.window_start > existing.window_start;
    if (!path_changed && !expanded && !narrowed)
        return existing;

    if (!path_changed && !expanded) {
        // The window slides forward on every sync. Only extend the cutoff and
        // skip the dropped head; FAILED/RETRY must keep attempts and backoff,
        // otherwise a persistent error would be retried every minute forever.
        const std::string completed_status =
            tierBackfillTaskStatusToString(TierBackfillTaskStatus::COMPLETED);
        existing.window_start = desired.window_start;
        existing.cutoff_time = std::max(existing.cutoff_time, desired.cutoff_time);
        existing.cursor_time = std::max(existing.cursor_time, desired.window_start);
        if (existing.status == completed_status && existing.cursor_time < existing.cutoff_time) {
            existing.status = tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING);
            existing.next_retry_at = 0;
            existing.completed_at = 0;
        } else if (existing.status != completed_status &&
                   existing.status != tierBackfillTaskStatusToString(TierBackfillTaskStatus::FAILED) &&
                   existing.cursor_time >= existing.cutoff_time) {
            existing.status = completed_status;
            existing.next_retry_at = 0;
            existing.completed_at = now;
        }
        existing.updated_at = now;
        if (!updateWindowAt(existing))
            return TierBackfillTask();
        return existing;
    }

    existing.timeline_path = desired.timeline_path;
    existing.window_start = desired.window_start;
    existing.cutoff_time = desired.cutoff_time;
    existing.cursor_time = (path_changed || expanded)
        ? desired.window_start
        : std::max(existing.cursor_time, desired.window_start);
    existing.status = existing.cursor_time >= existing.cutoff_time
        ? tierBackfillTaskStatusToString(TierBackfillTaskStatus::COMPLETED)
        : tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING);
    existing.attempt_count = 0;
    existing.next_retry_at = 0;
    existing.last_error.clear();
    existing.completed_at = existing.status == tierBackfillTaskStatusToString(TierBackfillTaskStatus::COMPLETED)
        ? now : 0;
    existing.updated_at = now;
    if (!updateDefinitionAt(existing))
        return TierBackfillTask();
    return existing;
}

std::vector<TierBackfillTask> TierBackfillTaskImp::claimNextDue() {
    return claimNextDueAt(static_cast<int64_t>(time(nullptr)));
}

void TierBackfillTaskImp::recoverRunning() {
    recoverRunningAt(static_cast<int64_t>(time(nullptr)));
}

bool TierBackfillTaskImp::retryFailed(const std::string &camera_id) {
    return retryFailedAt(camera_id, static_cast<int64_t>(time(nullptr)));
}

bool TierBackfillTaskImp::markFailure(const TierBackfillTask &task,
                                      const std::string &error,
                                      int64_t scanned_blocks,
                                      int64_t orphan_blocks,
                                      int max_attempts,
                                      int retry_base_seconds) {
    int attempts = task.attempt_count + 1;
    if (max_attempts <= 0)
        max_attempts = 5;
    if (retry_base_seconds <= 0)
        retry_base_seconds = 30;
    bool terminal = attempts >= max_attempts;
    int exponent = std::min(attempts - 1, 10);
    int64_t delay = static_cast<int64_t>(retry_base_seconds) << exponent;
    delay = std::min<int64_t>(delay, 3600);
    int64_t now = static_cast<int64_t>(time(nullptr));
    return markFailureAt(
        task,
        terminal ? tierBackfillTaskStatusToString(TierBackfillTaskStatus::FAILED)
                 : tierBackfillTaskStatusToString(TierBackfillTaskStatus::RETRY),
        attempts, terminal ? 0 : now + delay, error,
        scanned_blocks, orphan_blocks, now);
}

bool TierBackfillTaskImp::commitChunk(const TierBackfillTask &task,
                                      const TierBackfillChunk &chunk) {
    int64_t now = static_cast<int64_t>(time(nullptr));
    bool completed = chunk.end_time >= task.cutoff_time;
    return commitChunkAt(
        task, chunk,
        completed ? tierBackfillTaskStatusToString(TierBackfillTaskStatus::COMPLETED)
                  : tierBackfillTaskStatusToString(TierBackfillTaskStatus::PENDING),
        completed ? now : 0, now);
}

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
