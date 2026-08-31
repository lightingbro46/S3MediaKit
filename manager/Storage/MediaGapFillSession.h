#ifndef STORAGE_MEDIA_GAP_FILL_SESSION_H
#define STORAGE_MEDIA_GAP_FILL_SESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "DbSchema.h"
#include "DbStorage.h"

namespace managerkit {

namespace MediaGapDiscoveryState {
static const char *const Pending = "Pending";
static const char *const Discovering = "Discovering";
static const char *const Ready = "Ready";
static const char *const NoMedia = "NoMedia";
static const char *const Failed = "Failed";
static const char *const Completed = "Completed";
}

namespace MediaGapControlState {
static const char *const Active = "Active";
static const char *const Paused = "Paused";
}

struct MediaGapFillSession {
    std::string device_id;
    std::string gap_id;
    std::string session_id;
    std::string source_type;
    uint64_t disconnect_time = 0;
    uint64_t reconnect_time = 0;
    std::string discovery_state = MediaGapDiscoveryState::Pending;
    std::string control_state = MediaGapControlState::Active;
    int discovery_attempts = 0;
    std::string last_error;
    uint64_t updated_time = 0;
};

DECLARE_ENTITY(MediaGapFillSession, "media_gap_fill_session",
    MAKE_PK("device_id", "gap_id"),
    &MediaGapFillSession::device_id, "device_id",
    &MediaGapFillSession::gap_id, "gap_id",
    &MediaGapFillSession::session_id, "session_id",
    &MediaGapFillSession::source_type, "source_type",
    &MediaGapFillSession::disconnect_time, "disconnect_time",
    &MediaGapFillSession::reconnect_time, "reconnect_time",
    &MediaGapFillSession::discovery_state, "discovery_state",
    &MediaGapFillSession::control_state, "control_state",
    &MediaGapFillSession::discovery_attempts, "discovery_attempts",
    &MediaGapFillSession::last_error, "last_error",
    &MediaGapFillSession::updated_time, "updated_time"
)

class MediaGapFillSessionRepository : public SqliteRepository<MediaGapFillSession> {
public:
    MediaGapFillSessionRepository()
        : SqliteRepository<MediaGapFillSession>(Database::kMediaServerDb) {}
};

class MediaGapFillSessionImpl : public MediaGapFillSessionRepository {
public:
    bool claimDiscovery(MediaGapFillSession &session, uint64_t now) {
        const std::string previous_state = session.discovery_state;
        const uint64_t previous_updated_time = session.updated_time;
        auto query = toolkit::QueryBuilder()
                         .update(EntityTraits<MediaGapFillSession>::tableName())
                         .set({{"discovery_state", MediaGapDiscoveryState::Discovering},
                               {"control_state", MediaGapControlState::Active},
                               {"discovery_attempts", std::to_string(session.discovery_attempts + 1)},
                               {"last_error", ""},
                               {"updated_time", std::to_string(now)}})
                         .where("device_id = ? AND gap_id = ? AND discovery_state = ? AND updated_time = ?",
                                {session.device_id, session.gap_id, previous_state,
                                 std::to_string(previous_updated_time)});
        if (!getExecutor()->execDML(query)) return false;
        session.discovery_state = MediaGapDiscoveryState::Discovering;
        session.control_state = MediaGapControlState::Active;
        ++session.discovery_attempts;
        session.last_error.clear();
        session.updated_time = now;
        return true;
    }

    bool addOrUpdate(const MediaGapFillSession &session) {
        if (session.device_id.empty() || session.gap_id.empty()) return false;
        return find(session.device_id, session.gap_id).empty()
            ? save(session, true) : updateById(session);
    }

    bool addOrUpdateWithTxn(const MediaGapFillSession &session,
                            const toolkit::SqliteTransaction::Ptr &txn) {
        if (session.device_id.empty() || session.gap_id.empty() || !txn) return false;
        MediaGapFillSession key;
        key.device_id = session.device_id;
        key.gap_id = session.gap_id;
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MediaGapFillSession>::getColumns())
                         .from(EntityTraits<MediaGapFillSession>::tableName())
                         .where("device_id = ? AND gap_id = ?", {session.device_id, session.gap_id});
        return executeRawWithTxn(txn, query).empty()
            ? saveWithTxn(session, txn, true)
            : updateByIdWithTxn(session, txn);
    }

    std::vector<MediaGapFillSession> find(const std::string &device_id,
                                          const std::string &gap_id) {
        MediaGapFillSession key;
        key.device_id = device_id;
        key.gap_id = gap_id;
        return findById(key);
    }

    std::vector<MediaGapFillSession> findByDevice(const std::string &device_id) {
        auto query = toolkit::QueryBuilder()
                         .select(EntityTraits<MediaGapFillSession>::getColumns())
                         .from(EntityTraits<MediaGapFillSession>::tableName())
                         .where("device_id = ?", {device_id})
                         .orderBy("disconnect_time ASC, gap_id ASC");
        auto rows = getExecutor()->executeRaw(query);
        std::vector<MediaGapFillSession> result;
        for (const auto &row : rows)
            result.emplace_back(EntityTraits<MediaGapFillSession>::fromRow(row));
        return result;
    }

    bool removeByDeviceAndGapWithTxn(const std::string &device_id,
                                     const std::string &gap_id,
                                     const toolkit::SqliteTransaction::Ptr &txn) {
        if (device_id.empty() || gap_id.empty() || !txn) return false;
        auto query = toolkit::QueryBuilder()
                         .deleteFrom(EntityTraits<MediaGapFillSession>::tableName())
                         .where("device_id = ? AND gap_id = ?", {device_id, gap_id});
        return execDMLWithTxn(txn, query);
    }
};

} // namespace managerkit

#endif // STORAGE_MEDIA_GAP_FILL_SESSION_H
