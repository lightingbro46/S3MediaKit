#ifndef CAMERA_DISCONNECTSESSIONMANAGER_H
#define CAMERA_DISCONNECTSESSIONMANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include "CameraController.h"
#include "Storage/SdSyncStream.h"
#include "Storage/SdSyncSession.h"

namespace managerkit {

namespace ConnectionHandlingState {
    static const char* const Initializing = "Initializing"; // Newly created session with no segments
    static const char* const Pending = "Pending";
    static const char* const Processing = "Processing";
    static const char* const Success = "Success";
    static const char* const Failed = "Failed";             // At least one segment/stream failed
}

struct RecordingVideoSegment {
    int32_t segment_index = -1;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    std::string state = ConnectionHandlingState::Pending;
    uint64_t processed_up_to = 0;
    bool dirty = true;

    static RecordingVideoSegment from(const SdSyncSegment& seg) {
        RecordingVideoSegment rec;
        rec.segment_index    = seg.segment_index;
        rec.start_time       = seg.start_time;
        rec.end_time         = seg.end_time;
        rec.state            = seg.state;
        rec.processed_up_to  = seg.processed_up_to;
        rec.dirty            = false;
        return rec;
    }

    SdSyncSegment toSdSyncSegment(const std::string& device_id,
                        const std::string& session_id,
                        const std::string& stream_id) const {
        SdSyncSegment seg;
        seg.device_id       = device_id;
        seg.session_id      = session_id;
        seg.stream_id       = stream_id;
        seg.segment_index   = segment_index;
        seg.start_time      = start_time;
        seg.end_time        = end_time;
        seg.state           = state;
        seg.processed_up_to = processed_up_to;
        return seg;
    }
};

struct StreamInfo {
    std::string stream_id;
    std::string replay_uri;
    uint64_t earliest_record_time = 0;
    uint64_t latest_record_time   = 0;
    bool dirty = true;

    static StreamInfo from(const SdSyncStream& stream) {
        StreamInfo info;
        info.stream_id            = stream.stream_id;
        info.replay_uri           = stream.replay_uri;
        info.earliest_record_time = stream.earliest_record_time;
        info.latest_record_time   = stream.latest_record_time;
        info.dirty                = false;
        return info;
    }

    SdSyncStream toSdSyncStream(const std::string& device_id) const {
        SdSyncStream stream;
        stream.device_id            = device_id;
        stream.stream_id            = stream_id;
        stream.replay_uri           = replay_uri;
        stream.earliest_record_time = earliest_record_time;
        stream.latest_record_time   = latest_record_time;
        return stream;
    }
};

struct StreamHandleInfo {
    std::string stream_id;
    std::string handle_state = ConnectionHandlingState::Pending;
    int32_t current_segment_index = -1;
    std::vector<RecordingVideoSegment> segments;

    bool meta_dirty = true;
    bool hasDirtyChild() const {
        for (size_t i = 0; i < segments.size(); ++i)
            if (segments[i].dirty) return true;
        return false;
    }

    static StreamHandleInfo from(const SdSyncStreamProgress& progress,
                                  const std::vector<SdSyncSegment>& segs) {
        StreamHandleInfo info;
        info.stream_id             = progress.stream_id;
        info.handle_state          = progress.handle_state;
        info.current_segment_index = progress.current_segment_index;
        info.meta_dirty            = false;
        info.segments.reserve(segs.size());
        for (const auto& seg : segs)
            info.segments.push_back(RecordingVideoSegment::from(seg));
        return info;
    }

    SdSyncStreamProgress toSdSyncStreamProgress(const std::string& device_id,
                                                 const std::string& session_id) const {
        SdSyncStreamProgress progress;
        progress.device_id             = device_id;
        progress.session_id            = session_id;
        progress.stream_id             = stream_id;
        progress.handle_state          = handle_state;
        progress.current_segment_index = current_segment_index;
        return progress;
    }

    std::vector<SdSyncSegment> toSdSyncSegments(const std::string& device_id,
                                                 const std::string& session_id) const {
        std::vector<SdSyncSegment> segs;
        segs.reserve(segments.size());
        for (const auto& seg : segments)
            segs.push_back(seg.toSdSyncSegment(device_id, session_id, stream_id));
        return segs;
    }
};

struct DisconnectSession {
    std::string session_id;
    uint64_t disconnect_time = 0;
    uint64_t reconnect_time = 0;
    uint64_t processing_start_time = 0;
    uint64_t processing_end_time = 0;
    std::string session_state = ConnectionHandlingState::Initializing;
    double progress_percent = 0.0;
    std::vector<StreamHandleInfo> stream_handle_infos;
    bool meta_dirty = true;

    bool isInitializing() const {
        return session_state == ConnectionHandlingState::Initializing;
    }

    bool hasDirtyChild() const {
        for (size_t i = 0; i < stream_handle_infos.size(); ++i) {
            const StreamHandleInfo& sh = stream_handle_infos[i];
            if (sh.meta_dirty || sh.hasDirtyChild()) return true;
        }
        return false;
    }

    static DisconnectSession from(const SdSyncSession& session,
                                   const std::vector<SdSyncStreamProgress>& progresses,
                                   const std::vector<std::vector<SdSyncSegment>>& segmentsPerStream) {
        DisconnectSession ds = from(session);
        ds.stream_handle_infos.reserve(progresses.size());
        for (size_t i = 0; i < progresses.size(); ++i)
            ds.stream_handle_infos.push_back(
                StreamHandleInfo::from(progresses[i], segmentsPerStream[i]));
        return ds;
    }

    SdSyncSession toSdSyncSession(const std::string& device_id) const {
        SdSyncSession session;
        session.device_id             = device_id;
        session.session_id            = session_id;
        session.disconnect_time       = disconnect_time;
        session.reconnect_time        = reconnect_time;
        session.processing_start_time = processing_start_time;
        session.processing_end_time   = processing_end_time;
        session.session_state         = session_state;
        session.progress_percent      = static_cast<float>(progress_percent);
        return session;
    }

    std::vector<SdSyncStreamProgress> toSdSyncStreamProgresses(const std::string& device_id) const {
        std::vector<SdSyncStreamProgress> out;
        out.reserve(stream_handle_infos.size());
        for (const auto& sh : stream_handle_infos)
            out.push_back(sh.toSdSyncStreamProgress(device_id, session_id));
        return out;
    }

    std::vector<SdSyncSegment> toSdSyncSegments(const std::string& device_id) const {
        std::vector<SdSyncSegment> out;
        for (const auto& sh : stream_handle_infos) {
            auto segs = sh.toSdSyncSegments(device_id, session_id);
            out.insert(out.end(), segs.begin(), segs.end());
        }
        return out;
    }

private:
    static DisconnectSession from(const SdSyncSession& session) {
        DisconnectSession ds;
        ds.session_id            = session.session_id;
        ds.disconnect_time       = session.disconnect_time;
        ds.reconnect_time        = session.reconnect_time;
        ds.processing_start_time = session.processing_start_time;
        ds.processing_end_time   = session.processing_end_time;
        ds.session_state         = session.session_state;
        ds.progress_percent      = session.progress_percent;
        ds.meta_dirty            = false;
        return ds;
    }
};

class DisconnectSessionManager {
public:
    DisconnectSessionManager();

    // Stream metadata management
    void setStreams(const std::vector<StreamInfo>& streams, const bool& isInit = false);
    std::vector<StreamInfo>& streams() { return _streams; }

    // Session management
    void setSessions(const std::vector<DisconnectSession>& sessions);
    std::vector<DisconnectSession>& sessions() { return _sessions; }

    const std::vector<DisconnectSession>& drainDeletedSessions() const { return _pendingDeletedSessions; }

    // get session failed or pending
    const std::vector<DisconnectSession> getRemainingSegments() const;

    // Connection lifecycle events
    void onDisconnected(uint64_t disconnect_time);
    bool onReconnected(uint64_t reconnect_time);

    // Query helpers
    bool isStreamEmpty() const { return _streams.empty(); }
    bool isSessionEmpty() const { return _sessions.empty(); }

    // Returns the session currently in Processing state
    DisconnectSession* getProcessingSession();

    // Returns the first session in Pending state
    DisconnectSession* getNextPendingSession();

    // Marks the first Pending session as Processing
    // Also updates all Pending streams/segments to Processing
    void markNextPendingProcessing();

    // Returns the session currently in Initializing state
    DisconnectSession* getInitializingSession();

    // Updates processing progress of a segment
    // Also updates current_segment_index of the corresponding stream
    void updateSegmentProgress(const std::string& stream_id, uint64_t processed_up_to, int32_t segment_index);
    void updateSegmentProgress(const std::string& stream_id, uint64_t processed_up_to, bool isShutdown = false);

    // Insert segments into a session
    void insertSegments(DisconnectSession& session, const std::unordered_map<std::string, std::vector<RecordingVideoSegment>>& segments);

    // Returns the current processing segment of a stream
    RecordingVideoSegment* getCurrentSegment(const std::string& stream_id);

    // remove session
    void removeSession(const DisconnectSession& session);

    // Returns replay URI of a stream
    const std::string getReplayURL(const std::string& stream_id) const;

    // Returns earliest available recording timestamp of a stream
    uint64_t getEarliestRecordTime(const std::string& stream_id) const;

    // Returns the next Pending segment in a stream
    // Returns nullptr if no pending segment exists
    RecordingVideoSegment* getNextPendingSegment(const std::string& stream_id);

    // Resets a Failed session for retry
    // Failed streams/segments become Pending
    bool resetFailedSession(const std::string& session_id);

    // Resets a Failed segment for individual retry
    // Related stream/session states may also revert to Pending
    bool resetFailedSegment(const std::string& session_id, const std::string& stream_id, int32_t segment_index);

    bool stopSession();

    bool stopSegment(const std::string& stream_id, int32_t segment_index);
private:
    // Creates and inserts a new session
    void push(uint64_t disconnect_time, uint64_t reconnect_time);

    // Sorts sessions by disconnect_time (ascending)
    void sortSessions();

    // Sorts stream segments by start_time (ascending)
    void sortSegments(StreamHandleInfo& info);

    // Calculates stream progress using duration-weighted segments
    double calcStreamProgress(const StreamHandleInfo& info) const;

    // Recalculates processing progress (%) of the processing session
    void updateSessionProgress();

    void sync();
private:
    std::vector<StreamInfo> _streams;
    std::vector<DisconnectSession> _sessions;
    std::vector<DisconnectSession> _pendingDeletedSessions;
    bool _waiting_reconnect  = false;
    uint64_t _pending_disconnect = 0;
};

} // namespace managerkit

#endif // CAMERA_DISCONNECTSESSIONMANAGER_H