#ifndef S3MANAGERKIT_SDCARDSYNCSTREAM_H
#define S3MANAGERKIT_SDCARDSYNCSTREAM_H

#include "DbStorage.h"
#include "Util/util.h"
#include <string>
#include <vector>

namespace managerkit {

struct SdSyncStream {
    std::string device_id;
    std::string stream_id;
    std::string replay_uri;
    uint64_t    earliest_record_time;
    uint64_t    latest_record_time;
};

static std::vector<std::string> stream_key = {"device_id", "stream_id"};

DECLARE_ENTITY(SdSyncStream, "sd_sync_stream",
    { stream_key },
    &SdSyncStream::device_id, "device_id",
    &SdSyncStream::stream_id, "stream_id",
    &SdSyncStream::replay_uri, "replay_uri",
    &SdSyncStream::earliest_record_time, "earliest_record_time",
    &SdSyncStream::latest_record_time, "latest_record_time"
)

class SdSyncStreamRepository : public SqliteRepository<SdSyncStream> {
public:
    SdSyncStreamRepository() : SqliteRepository<SdSyncStream>(Database::kMediaServerDb) {}

protected:
    std::vector<SdSyncStream> queryByDevice(const std::string& deviceId) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<SdSyncStream>::getColumns())
                         .from(EntityTraits<SdSyncStream>::tableName())
                         .where("device_id = ?", { deviceId });
        auto rows = _executor->executeRaw(query);
        std::vector<SdSyncStream> ret;
        for (const auto& row : rows)
            ret.push_back(EntityTraits<SdSyncStream>::fromRow(row));
        return ret;
    }
};

class SdSyncStreamImp : public SdSyncStreamRepository {
public:
    using Ptr = std::shared_ptr<SdSyncStreamImp>;
    SdSyncStreamImp() : SdSyncStreamRepository() {}

    bool addOrUpdate(const SdSyncStream& stream) {
        if (!SdSyncStreamRepository::findById(stream).empty()) {
            updateById(stream);
            return true;
        } else {
            return save(stream, true);
        }
    }

    void remove(const SdSyncStream& stream) {
        removeById(stream);
    }

    std::vector<SdSyncStream> findByDevice(const std::string& deviceId) {
        return queryByDevice(deviceId);
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_SDCARDSYNCSTREAM_H