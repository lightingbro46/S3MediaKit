#ifndef STORAGE_TIER_BACKFILL_TASK_H
#define STORAGE_TIER_BACKFILL_TASK_H

#ifdef ENABLE_TIER_STORAGE

#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"
#include "DbSchema.h"
#include "TieringJob.h"

namespace managerkit {

enum class TierBackfillTaskStatus {
    PENDING,
    RUNNING,
    RETRY,
    COMPLETED,
    FAILED
};

const char *tierBackfillTaskStatusToString(TierBackfillTaskStatus status);

struct TierBackfillTask {
    std::string task_id;
    std::string camera_id;
    std::string timeline_path;
    int64_t window_start = 0;
    int64_t cutoff_time = 0;
    int64_t cursor_time = 0;
    std::string status;
    int attempt_count = 0;
    int64_t next_retry_at = 0;
    std::string last_error;
    int64_t scanned_blocks = 0;
    int64_t registered_blocks = 0;
    int64_t orphan_blocks = 0;
    int64_t indexed_bytes = 0;
    int64_t started_at = 0;
    int64_t completed_at = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;

    Json::Value toJson() const;
};

DECLARE_ENTITY(TierBackfillTask, "tier_backfill_tasks",
    {"task_id"},
    &TierBackfillTask::task_id,           "task_id",
    &TierBackfillTask::camera_id,         "camera_id",
    &TierBackfillTask::timeline_path,     "timeline_path",
    &TierBackfillTask::window_start,      "window_start",
    &TierBackfillTask::cutoff_time,       "cutoff_time",
    &TierBackfillTask::cursor_time,       "cursor_time",
    &TierBackfillTask::status,            "status",
    &TierBackfillTask::attempt_count,     "attempt_count",
    &TierBackfillTask::next_retry_at,     "next_retry_at",
    &TierBackfillTask::last_error,        "last_error",
    &TierBackfillTask::scanned_blocks,    "scanned_blocks",
    &TierBackfillTask::registered_blocks, "registered_blocks",
    &TierBackfillTask::orphan_blocks,     "orphan_blocks",
    &TierBackfillTask::indexed_bytes,     "indexed_bytes",
    &TierBackfillTask::started_at,        "started_at",
    &TierBackfillTask::completed_at,      "completed_at",
    &TierBackfillTask::created_at,        "created_at",
    &TierBackfillTask::updated_at,        "updated_at"
)

struct TierBackfillChunk {
    int64_t start_time = 0;
    int64_t end_time = 0;
    int64_t scanned_blocks = 0;
    int64_t registered_blocks = 0;
    int64_t orphan_blocks = 0;
    int64_t indexed_bytes = 0;
    std::vector<SegmentTierRange> ranges;
};

class TierBackfillTaskRepository : public SqliteRepository<TierBackfillTask> {
public:
    TierBackfillTaskRepository();

    std::vector<TierBackfillTask> findByTaskId(const std::string &task_id);
    std::vector<TierBackfillTask> findByCameraId(const std::string &camera_id);
    std::vector<TierBackfillTask> findAll();
    bool insertTask(const TierBackfillTask &task);
    bool updateDefinitionAt(const TierBackfillTask &task);
    bool retryFailedAt(const std::string &camera_id, int64_t updated_at);
    bool hasDueAt(int64_t now);
    std::vector<TierBackfillTask> claimNextDueAt(int64_t now);
    void recoverRunningAt(int64_t updated_at);
    bool markFailureAt(const TierBackfillTask &task,
                       const std::string &status,
                       int attempt_count,
                       int64_t next_retry_at,
                       const std::string &error,
                       int64_t scanned_blocks,
                       int64_t orphan_blocks,
                       int64_t updated_at);
    bool commitChunkAt(const TierBackfillTask &task,
                       const TierBackfillChunk &chunk,
                       const std::string &status,
                       int64_t completed_at,
                       int64_t updated_at);
};

class TierBackfillTaskImp : public TierBackfillTaskRepository {
public:
    TierBackfillTask ensureTask(TierBackfillTask desired);
    std::vector<TierBackfillTask> claimNextDue();
    void recoverRunning();
    bool retryFailed(const std::string &camera_id);
    bool markFailure(const TierBackfillTask &task,
                     const std::string &error,
                     int64_t scanned_blocks,
                     int64_t orphan_blocks,
                     int max_attempts,
                     int retry_base_seconds);
    bool commitChunk(const TierBackfillTask &task,
                     const TierBackfillChunk &chunk);
};

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE

#endif // STORAGE_TIER_BACKFILL_TASK_H
