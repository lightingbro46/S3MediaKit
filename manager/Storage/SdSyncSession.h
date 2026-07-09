#ifndef S3MANAGERKIT_SDCARDSYNCSESSION_H
#define S3MANAGERKIT_SDCARDSYNCSESSION_H

#include "DbStorage.h"
#include "Util/util.h"
#include <string>
#include <vector>
#include "SdSyncSegment.h"
#include "SdSyncStreamProgress.h"

namespace managerkit {

struct SdSyncSession {
    std::string device_id;
    std::string session_id;
    uint64_t    disconnect_time;
    uint64_t    reconnect_time;
    uint64_t    processing_start_time;
    uint64_t    processing_end_time;
    float       progress_percent;
    std::string session_state;
};

static std::vector<std::string> ses_key = {"device_id", "session_id"};

DECLARE_ENTITY(SdSyncSession, "sd_sync_session",
    { ses_key },
    &SdSyncSession::device_id, "device_id",
    &SdSyncSession::session_id, "session_id",
    &SdSyncSession::disconnect_time, "disconnect_time",
    &SdSyncSession::reconnect_time, "reconnect_time",
    &SdSyncSession::processing_start_time, "processing_start_time",
    &SdSyncSession::processing_end_time, "processing_end_time",
    &SdSyncSession::progress_percent, "progress_percent",
    &SdSyncSession::session_state, "session_state"
)

class SdSyncSessionRepository : public SqliteRepository<SdSyncSession> {
public:
    SdSyncSessionRepository() : SqliteRepository<SdSyncSession>(Database::kMediaServerDb) {}

protected:
    std::vector<SdSyncSession> queryByDevice(const std::string& deviceId) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<SdSyncSession>::getColumns())
                         .from(EntityTraits<SdSyncSession>::tableName())
                         .where("device_id = ?", { deviceId });
        auto rows = _executor->executeRaw(query);
        std::vector<SdSyncSession> ret;
        for (const auto& row : rows)
            ret.push_back(EntityTraits<SdSyncSession>::fromRow(row));
        return ret;
    }
};

class SdSyncSessionImp : public SdSyncSessionRepository {
public:
    using Ptr = std::shared_ptr<SdSyncSessionImp>;
    SdSyncSessionImp() : SdSyncSessionRepository() {
        _streamProgressImp= std::make_shared<SdSyncStreamProgressImp>();
        _segmentImp = std::make_shared<SdSyncSegmentImp>();
    }

    bool addOrUpdate(const SdSyncSession& session) {
        if (!SdSyncSessionRepository::findById(session).empty()) {
            updateById(session);
            return true;
        } else {
            return save(session, true);
        }
    }

    bool addOrUpdateStreamInfo(const SdSyncStreamProgress& progress) {
        return _streamProgressImp->addOrUpdate(progress);
    }

    bool addOrUpdateSegment(const SdSyncSegment& segment) {
        return _segmentImp->addOrUpdate(segment);
    }

    void remove(const SdSyncSession& session) {
        removeById(session);
    }

    void removeStreamInfo(const SdSyncStreamProgress& progress) {
        _streamProgressImp->remove(progress);
    }

    void removeSegment(const SdSyncSegment& segment) {
        _segmentImp->remove(segment);
    }

    std::vector<SdSyncSession> findByDevice(const std::string& deviceId) {
        return queryByDevice(deviceId);
    }

    std::vector<SdSyncStreamProgress> findStreamInfoBySession(const std::string& deviceId, const std::string& sessionId) {
        return _streamProgressImp->findBySession(deviceId, sessionId);
    }

    std::vector<SdSyncSegment> findSegmentByStream(const std::string& deviceId,
                                                        const std::string& sessionId,
                                                        const std::string& streamId) {
        return _segmentImp->findByStream(deviceId, sessionId, streamId);
    }
private:
    SdSyncStreamProgressImp::Ptr _streamProgressImp;
    SdSyncSegmentImp::Ptr _segmentImp;
};

} // namespace managerkit

#endif // S3MANAGERKIT_SDCARDSYNCSESSION_H