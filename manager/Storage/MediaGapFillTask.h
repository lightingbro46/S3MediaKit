#ifndef STORAGE_MEDIA_GAP_FILL_TASK_H
#define STORAGE_MEDIA_GAP_FILL_TASK_H

#include <algorithm>
#include <string>
#include <vector>
#include <json/json.h>
#include "DbStorage.h"
#include "DbSchema.h"
#include "Util/util.h"
#include "Common/StrUtil.h"

namespace managerkit {

struct MediaGapFillTask {
    std::string connection_id;
    std::string gap_id;
    std::string session_id;
    std::string device_id;
    std::string stream_id;
    std::string source_type;
    std::string origin_url;
    uint64_t source_begin_time = 0;
    int32_t segment_index = -1;
    uint64_t disconnect_time = 0;
    uint64_t reconnect_time = 0;
    uint64_t begin_time = 0;
    uint64_t end_time = 0;
    uint64_t processed_up_to = 0;
    uint64_t processing_start_time = 0;
    uint64_t processing_end_time = 0;
    int retry_count = 0;
    int retry_limit = 0;
    uint64_t next_retry_time = 0;
    uint64_t last_attempt_time = 0;
    std::string state;
    std::string last_error;
    uint64_t updated_time = 0;
};

DECLARE_ENTITY(MediaGapFillTask, "media_gap_fill_task",
    {"connection_id"},
    &MediaGapFillTask::connection_id, "connection_id",
    &MediaGapFillTask::gap_id, "gap_id",
    &MediaGapFillTask::session_id, "session_id",
    &MediaGapFillTask::device_id, "device_id",
    &MediaGapFillTask::stream_id, "stream_id",
    &MediaGapFillTask::source_type, "source_type",
    &MediaGapFillTask::origin_url, "origin_url",
    &MediaGapFillTask::source_begin_time, "source_begin_time",
    &MediaGapFillTask::segment_index, "segment_index",
    &MediaGapFillTask::disconnect_time, "disconnect_time",
    &MediaGapFillTask::reconnect_time, "reconnect_time",
    &MediaGapFillTask::begin_time, "begin_time",
    &MediaGapFillTask::end_time, "end_time",
    &MediaGapFillTask::processed_up_to, "processed_up_to",
    &MediaGapFillTask::processing_start_time, "processing_start_time",
    &MediaGapFillTask::processing_end_time, "processing_end_time",
    &MediaGapFillTask::retry_count, "retry_count",
    &MediaGapFillTask::retry_limit, "retry_limit",
    &MediaGapFillTask::next_retry_time, "next_retry_time",
    &MediaGapFillTask::last_attempt_time, "last_attempt_time",
    &MediaGapFillTask::state, "state",
    &MediaGapFillTask::last_error, "last_error",
    &MediaGapFillTask::updated_time, "updated_time"
)

class MediaGapFillTaskRepository : public SqliteRepository<MediaGapFillTask> {
public:
    MediaGapFillTaskRepository() : SqliteRepository<MediaGapFillTask>(Database::kMediaServerDb) {}

};

class MediaGapFillTaskImpl : public MediaGapFillTaskRepository {
public:
    MediaGapFillTaskImpl() : MediaGapFillTaskRepository() {}

    bool addOrUpdate(const MediaGapFillTask &task) {
        if (task.connection_id.empty()) {
            WarnL << "Cannot add/update task with empty id";
            return false;
        }
        auto existing = MediaGapFillTaskRepository::findById(task);
        if (!existing.empty()) {
            return updateById(task);
        } else {
            return save(task, true);
        }
    }

    bool addOrUpdateWithTxn(const MediaGapFillTask &task,
                            const toolkit::SqliteTransaction::Ptr &txn) {
        if (task.connection_id.empty() || !txn) return false;
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MediaGapFillTask>::getColumns())
                         .from(EntityTraits<MediaGapFillTask>::tableName())
                         .where("connection_id = ?", {task.connection_id});
        return executeRawWithTxn(txn, query).empty()
            ? saveWithTxn(task, txn, true)
            : updateByIdWithTxn(task, txn);
    }

    std::vector<MediaGapFillTask> findById(const std::string &id) {
        MediaGapFillTask key;
        key.connection_id = id;
        return MediaGapFillTaskRepository::findById(key);
    }

    std::vector<MediaGapFillTask> findByDevice(const std::string &device_id) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MediaGapFillTask>::getColumns())
                         .from(EntityTraits<MediaGapFillTask>::tableName())
                         .where("device_id = ?", {device_id})
                         .orderBy("begin_time ASC, connection_id ASC");
        auto rows = getExecutor()->executeRaw(query);
        std::vector<MediaGapFillTask> result;
        for (const auto &row : rows)
            result.emplace_back(EntityTraits<MediaGapFillTask>::fromRow(row));
        return result;
    }

    std::vector<MediaGapFillTask> findByDeviceAndSession(const std::string &device_id,
                                                          const std::string &session_id) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MediaGapFillTask>::getColumns())
                         .from(EntityTraits<MediaGapFillTask>::tableName())
                         .where("device_id = ? AND session_id = ?", {device_id, session_id})
                         .orderBy("stream_id ASC, segment_index ASC, begin_time ASC");
        auto rows = getExecutor()->executeRaw(query);
        std::vector<MediaGapFillTask> result;
        result.reserve(rows.size());
        for (const auto &row : rows)
            result.emplace_back(EntityTraits<MediaGapFillTask>::fromRow(row));
        return result;
    }

    std::vector<MediaGapFillTask> findByDeviceAndGap(const std::string &device_id,
                                                     const std::string &gap_id) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MediaGapFillTask>::getColumns())
                         .from(EntityTraits<MediaGapFillTask>::tableName())
                         .where("device_id = ? AND gap_id = ?", {device_id, gap_id})
                         .orderBy("stream_id ASC, segment_index ASC, begin_time ASC");
        auto rows = getExecutor()->executeRaw(query);
        std::vector<MediaGapFillTask> result;
        for (const auto &row : rows)
            result.emplace_back(EntityTraits<MediaGapFillTask>::fromRow(row));
        return result;
    }

    bool removeByDeviceAndGapWithTxn(const std::string &device_id,
                                     const std::string &gap_id,
                                     const toolkit::SqliteTransaction::Ptr &txn) {
        if (device_id.empty() || gap_id.empty() || !txn) return false;
        auto query = toolkit::QueryBuilder()
                         .deleteFrom(EntityTraits<MediaGapFillTask>::tableName())
                         .where("device_id = ? AND gap_id = ?", {device_id, gap_id});
        return execDMLWithTxn(txn, query);
    }
};

} // namespace managerkit

#endif // STORAGE_MEDIA_GAP_FILL_TASK_H
