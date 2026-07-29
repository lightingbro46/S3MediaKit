#ifndef S3MANAGERKIT_SDCARDSYNCSEGMENT_H
#define S3MANAGERKIT_SDCARDSYNCSEGMENT_H

#include "DbStorage.h"
#include "Util/util.h"
#include <string>
#include <vector>

namespace managerkit {

struct SdSyncSegment {
    std::string device_id;
    std::string session_id;
    std::string stream_id;
    int32_t     segment_index;
    uint64_t    start_time;
    uint64_t    end_time;
    uint64_t    processed_up_to;
    std::string state;
};

static std::vector<std::string> seg_key = {"device_id", "session_id", "stream_id", "segment_index"};

DECLARE_ENTITY(SdSyncSegment, "sd_sync_segment",
    { seg_key },
    &SdSyncSegment::device_id, "device_id",
    &SdSyncSegment::session_id, "session_id",
    &SdSyncSegment::stream_id, "stream_id",
    &SdSyncSegment::segment_index, "segment_index",
    &SdSyncSegment::start_time, "start_time",
    &SdSyncSegment::end_time, "end_time",
    &SdSyncSegment::processed_up_to, "processed_up_to",
    &SdSyncSegment::state, "state"
)

class SdSyncSegmentRepository : public SqliteRepository<SdSyncSegment> {
public:
    SdSyncSegmentRepository() : SqliteRepository<SdSyncSegment>(Database::kMediaServerDb) {}

protected:
    std::vector<SdSyncSegment> queryByStream(const std::string& deviceId,
                                             const std::string& sessionId,
                                             const std::string& streamId) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<SdSyncSegment>::getColumns())
                         .from(EntityTraits<SdSyncSegment>::tableName())
                         .where("device_id = ? AND session_id = ? AND stream_id = ?",
                                { deviceId, sessionId, streamId });
        auto rows = _executor->executeRaw(query);
        std::vector<SdSyncSegment> ret;
        for (const auto& row : rows)
            ret.push_back(EntityTraits<SdSyncSegment>::fromRow(row));
        return ret;
    }
};

class SdSyncSegmentImp : public SdSyncSegmentRepository {
public:
    using Ptr = std::shared_ptr<SdSyncSegmentImp>;
    SdSyncSegmentImp() : SdSyncSegmentRepository() {}

    bool addOrUpdate(const SdSyncSegment& segment) {
        if (!SdSyncSegmentRepository::findById(segment).empty()) {
            updateById(segment);
            return true;
        } else {
            return save(segment, true);
        }
    }

    void remove(const SdSyncSegment& segment) {
        removeById(segment);
    }

    std::vector<SdSyncSegment> findByStream(const std::string& deviceId,
                                            const std::string& sessionId,
                                            const std::string& streamId) {
        return queryByStream(deviceId, sessionId, streamId);
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_SDCARDSYNCSEGMENT_H