#ifndef S3MANAGERKIT_SDCARDSYNCSTREAMPROGRESS_H
#define S3MANAGERKIT_SDCARDSYNCSTREAMPROGRESS_H

#include "DbStorage.h"
#include "Util/util.h"
#include <string>
#include <vector>

namespace managerkit {

struct SdSyncStreamProgress {
    std::string device_id;
    std::string session_id;
    std::string stream_id;
    std::string handle_state;
    int32_t current_segment_index;
};

static std::vector<std::string> stream_progress_key = {"device_id", "session_id", "stream_id"};

DECLARE_ENTITY(SdSyncStreamProgress, "sd_sync_stream_progress",
    { stream_progress_key },
    &SdSyncStreamProgress::device_id, "device_id",
    &SdSyncStreamProgress::session_id, "session_id",
    &SdSyncStreamProgress::stream_id, "stream_id",
    &SdSyncStreamProgress::handle_state, "handle_state",
    &SdSyncStreamProgress::current_segment_index, "current_segment_index"
)

class SdSyncStreamProgressRepository : public SqliteRepository<SdSyncStreamProgress> {
public:
    SdSyncStreamProgressRepository() : SqliteRepository<SdSyncStreamProgress>(Database::kMediaServerDb) {}

protected:
    std::vector<SdSyncStreamProgress> queryBySession(const std::string& deviceId,
                                                     const std::string& sessionId) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<SdSyncStreamProgress>::getColumns())
                         .from(EntityTraits<SdSyncStreamProgress>::tableName())
                         .where("device_id = ? AND session_id = ?", { deviceId, sessionId });
        auto rows = _executor->executeRaw(query);
        std::vector<SdSyncStreamProgress> ret;
        for (const auto& row : rows)
            ret.push_back(EntityTraits<SdSyncStreamProgress>::fromRow(row));
        return ret;
    }
};

class SdSyncStreamProgressImp : public SdSyncStreamProgressRepository {
public:
    using Ptr = std::shared_ptr<SdSyncStreamProgressImp>;
    SdSyncStreamProgressImp() : SdSyncStreamProgressRepository() {}

    bool addOrUpdate(const SdSyncStreamProgress& progress) {
        if (!SdSyncStreamProgressRepository::findById(progress).empty()) {
            updateById(progress);
            return true;
        } else {
            return save(progress, true);
        }
    }

    void remove(const SdSyncStreamProgress& progress) {
        removeById(progress);
    }

    std::vector<SdSyncStreamProgress> findBySession(const std::string& deviceId,
                                                    const std::string& sessionId) {
        return queryBySession(deviceId, sessionId);
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_SDCARDSYNCSTREAMPROGRESS_H