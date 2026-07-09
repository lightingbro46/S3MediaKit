#include "DisconnectSessionManager.h"
#include "Common/config.h"
#include "json/json.h"

#include <algorithm>
#include <ctime>

using namespace std;

namespace managerkit {

DisconnectSessionManager::DisconnectSessionManager() {}

void DisconnectSessionManager::setStreams(const std::vector<StreamInfo>& streams, const bool& isInit) {
    if (streams.empty()) return;

    auto hasChanged = [](const StreamInfo& oldS, const StreamInfo& newS) {
        return oldS.replay_uri != newS.replay_uri ||
               oldS.earliest_record_time != newS.earliest_record_time ||
               oldS.latest_record_time != newS.latest_record_time;
    };

    if (isInit) {
        _streams = streams;
    } else {
        std::unordered_map<std::string, StreamInfo*> map;
        for (auto& s : _streams)
            map[s.stream_id] = &s;

        if (_streams.size() > streams.size()) {
            for (const auto& s : streams) {
                auto it = map.find(s.stream_id);
                if (it == map.end()) continue;

                StreamInfo* existing = it->second;
                bool changed = hasChanged(*existing, s);
                if (!changed) continue;

                *existing = s;
                existing->dirty = true;
            }
        } else {
            std::vector<StreamInfo> result;
            result.reserve(streams.size());

            for (const auto& s : streams) {
                auto it = map.find(s.stream_id);
                if (it != map.end()) {
                    StreamInfo* existing = it->second;
                    bool changed = hasChanged(*existing, s);
                    if (changed) {
                        existing->dirty = true;
                        *existing = s;
                        existing->dirty = true;
                    }
                    result.push_back(*existing);
                } else {
                    StreamInfo item = s;
                    item.dirty = true;
                    result.push_back(item);
                }
            }

            _streams = result;
        }
    }
}

void DisconnectSessionManager::setSessions(const std::vector<DisconnectSession>& sessions) {
    _sessions = sessions;
    sortSessions();
    for (auto& session : _sessions)
        for (auto& info : session.stream_handle_infos)
            sortSegments(info);
}

const std::vector<DisconnectSession> DisconnectSessionManager::getRemainingSegments() const {
    std::vector<DisconnectSession> sessions;
    for (const auto& session : _sessions)
        if (session.session_state == ConnectionHandlingState::Pending || session.session_state == ConnectionHandlingState::Failed)
            sessions.push_back(session);

    return sessions;
}

void DisconnectSessionManager::onDisconnected(uint64_t disconnect_time) {
    _waiting_reconnect = true;
    if (_pending_disconnect == 0)
        _pending_disconnect = disconnect_time;
}

bool DisconnectSessionManager::onReconnected(uint64_t reconnect_time) {
    if (!_waiting_reconnect) return false;
    if (reconnect_time <= _pending_disconnect) return false;

    uint64_t duration = reconnect_time - _pending_disconnect;
    // Ignore:
    // true -> false -> true
    if (duration < 120) {
        _waiting_reconnect = false;
        _pending_disconnect = 0;
        return false;
    }
    _waiting_reconnect = false;
    push(_pending_disconnect, reconnect_time);
    _pending_disconnect = 0;
    return true;
}

DisconnectSession* DisconnectSessionManager::getProcessingSession() {
    for (auto& session : _sessions)
        if (session.session_state == ConnectionHandlingState::Processing)
            return &session;
    return nullptr;
}

DisconnectSession* DisconnectSessionManager::getNextPendingSession() {
    for (auto& session : _sessions)
        if (session.session_state == ConnectionHandlingState::Pending)
            return &session;
    return nullptr;
}

void DisconnectSessionManager::markNextPendingProcessing() {
    DisconnectSession* session = getNextPendingSession();
    if (!session) return;

    session->session_state = ConnectionHandlingState::Processing;
    session->meta_dirty = true;
    if (session->processing_start_time == 0)
        session->processing_start_time = static_cast<uint64_t>(time(nullptr));

    for (auto& info : session->stream_handle_infos) {
        if (info.handle_state != ConnectionHandlingState::Pending) continue;

        info.handle_state = ConnectionHandlingState::Processing;
        info.meta_dirty = true;
        for (auto& seg : info.segments) {
            if (seg.state == ConnectionHandlingState::Pending) {
                info.current_segment_index = seg.segment_index;
                seg.state = ConnectionHandlingState::Processing;
                seg.dirty = true;
                break;
            }
        }
    }
}

DisconnectSession* DisconnectSessionManager::getInitializingSession() {
    for (auto& session : _sessions)
        if (session.session_state == ConnectionHandlingState::Initializing)
            return &session;
    return nullptr;
}

void DisconnectSessionManager::updateSegmentProgress(const std::string& stream_id, uint64_t processed_up_to, int32_t segment_index) {
    DisconnectSession* session = getProcessingSession();
    if (!session) return;

    for (auto& info : session->stream_handle_infos) {
        if (info.stream_id != stream_id) continue;

        info.current_segment_index = segment_index;
        info.handle_state = ConnectionHandlingState::Processing;
        info.meta_dirty = true;

        if (segment_index >= 0 && segment_index < static_cast<int32_t>(info.segments.size())) {
            auto& seg = info.segments[segment_index];
            seg.processed_up_to = processed_up_to;
            seg.state = ConnectionHandlingState::Processing;
            seg.dirty = true;
        }
        break;
    }

    updateSessionProgress();
}

void DisconnectSessionManager::updateSegmentProgress(const std::string& stream_id, uint64_t processed_up_to, bool isShutdown) {
    DisconnectSession* session = getProcessingSession();
    if (!session) return;

    bool seg_done = true;

    for (auto& info : session->stream_handle_infos) {
        if (info.stream_id != stream_id) continue;

        info.handle_state = ConnectionHandlingState::Processing;
        info.meta_dirty = true;

        int32_t idx = info.current_segment_index;
        if (idx >= 0 && idx < static_cast<int32_t>(info.segments.size())) {
            auto& seg = info.segments[idx];
            if (processed_up_to != static_cast<uint64_t>(seg.end_time) && processed_up_to < seg.processed_up_to) return;
            seg.processed_up_to = processed_up_to;
            seg.dirty = true;

            if (processed_up_to == static_cast<uint64_t>(seg.end_time)) {
                seg.state = ConnectionHandlingState::Success;
            }
            else if (isShutdown && processed_up_to != static_cast<uint64_t>(seg.end_time)) {
                seg.state = ConnectionHandlingState::Failed;
            }
            else {
                seg_done = false;
                seg.state = ConnectionHandlingState::Processing;
            }
        } else {
            seg_done = false;
        }
        break;
    }

    updateSessionProgress();

    if (seg_done)
        sync();
}

void DisconnectSessionManager::updateSessionProgress() {
    DisconnectSession* session = getProcessingSession();
    if (!session || session->stream_handle_infos.empty()) return;

    double total = 0.0;
    for (const auto& info : session->stream_handle_infos)
        total += calcStreamProgress(info);
    session->progress_percent = total / session->stream_handle_infos.size();
    session->meta_dirty = true;
}

void DisconnectSessionManager::insertSegments(DisconnectSession& session, const std::unordered_map<std::string, std::vector<RecordingVideoSegment>>& segments) {
    for (auto& stream : _streams) {
        auto it = segments.find(stream.stream_id);
        if (it == segments.end()) continue;
        if (it->second.empty()) continue;
        StreamHandleInfo info;
        info.stream_id = stream.stream_id;
        info.handle_state = ConnectionHandlingState::Pending;
        info.current_segment_index = -1;
        info.segments = it->second;
        sortSegments(info);

        for (int32_t i = 0; i < static_cast<int32_t>(info.segments.size()); ++i)
            info.segments[i].segment_index = i;

        session.stream_handle_infos.push_back(info);
    }

    // default dirty state of streams and segments is true. Do not reset it here
    if (session.session_state == ConnectionHandlingState::Initializing) {
        session.session_state = ConnectionHandlingState::Pending;
        session.meta_dirty = true;
    }
}

RecordingVideoSegment* DisconnectSessionManager::getCurrentSegment(const std::string& stream_id) {
    for (auto& session : _sessions) {
        if (session.session_state != ConnectionHandlingState::Processing) continue;
        for (auto& info : session.stream_handle_infos) {
            if (info.stream_id != stream_id) continue;
            int32_t idx = info.current_segment_index;
            if (idx >= 0 && idx < static_cast<int32_t>(info.segments.size()))
                return &info.segments[idx];
        }
    }
    return nullptr;
}

void DisconnectSessionManager::removeSession(const DisconnectSession& session) {
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        if (it->disconnect_time == session.disconnect_time
         && it->reconnect_time  == session.reconnect_time) {
            _pendingDeletedSessions.push_back(*it);
            _sessions.erase(it);
            break;
        }
    }
}

const std::string DisconnectSessionManager::getReplayURL(const std::string& stream_id) const {
    for (const auto& stream : _streams)
        if (stream.stream_id == stream_id)
            return stream.replay_uri;
    return "";
}

uint64_t DisconnectSessionManager::getEarliestRecordTime(const std::string& stream_id) const {
    for (const auto& stream : _streams)
        if (stream.stream_id == stream_id)
            return stream.earliest_record_time;
    return 0;
}

RecordingVideoSegment* DisconnectSessionManager::getNextPendingSegment(const std::string& stream_id) {
    DisconnectSession* session = getProcessingSession();
    if (!session) return nullptr;

    for (auto& info : session->stream_handle_infos) {
        if (info.stream_id != stream_id) continue;
        for (auto& seg : info.segments)
            if (seg.state == ConnectionHandlingState::Pending)
                return &seg;
    }
    return nullptr;
}

bool DisconnectSessionManager::resetFailedSession(const std::string& session_id) {
    DisconnectSession* target = nullptr;
    for (auto& s : _sessions)
        if (s.session_id == session_id) { target = &s; break; }

    if (!target) return false;
    if (target->session_state != ConnectionHandlingState::Failed) return false;

    target->session_state = ConnectionHandlingState::Pending;
    target->processing_end_time = 0;
    target->meta_dirty = true;

    for (auto& info : target->stream_handle_infos) {
        if (info.handle_state == ConnectionHandlingState::Failed) {
            info.handle_state = ConnectionHandlingState::Pending;
            info.current_segment_index = -1;
            info.meta_dirty = true;
        }
        for (auto& seg : info.segments) {
            if (seg.state == ConnectionHandlingState::Failed) {
                seg.state = ConnectionHandlingState::Pending;
                seg.dirty = true;
            }
        }
    }
    return true;
}

bool DisconnectSessionManager::resetFailedSegment(const std::string& session_id, const std::string& stream_id, int32_t segment_index)
{
    DisconnectSession* target_session = nullptr;
    for (auto& s : _sessions)
        if (s.session_id == session_id) { target_session = &s; break; }
    if (!target_session) return false;

    StreamHandleInfo* target_stream = nullptr;
    for (auto& info : target_session->stream_handle_infos)
        if (info.stream_id == stream_id) { target_stream = &info; break; }
    if (!target_stream) return false;

    if (segment_index < 0 || segment_index >= static_cast<int32_t>(target_stream->segments.size()))
        return false;

    RecordingVideoSegment& seg = target_stream->segments[segment_index];
    if (seg.state != ConnectionHandlingState::Failed) return false;

    seg.state = ConnectionHandlingState::Pending;
    seg.dirty = true;

    // Set stream state to Pending if it is not Processing
    if (target_stream->handle_state != ConnectionHandlingState::Processing) {
        target_stream->handle_state = ConnectionHandlingState::Pending;
        target_stream->current_segment_index = -1;
        target_stream->meta_dirty = true;
    }

    // Set session state to Pending if it is not Processing
    if (target_session->session_state != ConnectionHandlingState::Processing) {
        target_session->session_state = ConnectionHandlingState::Pending;
        target_session->processing_end_time = 0;
        target_session->meta_dirty = true;
    }

    return true;
}

bool DisconnectSessionManager::stopSession() {
    DisconnectSession* session = getProcessingSession();
    if (!session) return false;

    session->session_state = ConnectionHandlingState::Pending;
    session->processing_end_time = 0;
    session->meta_dirty = true;

    for (auto& info : session->stream_handle_infos) {
        if (info.handle_state == ConnectionHandlingState::Processing) {
            info.handle_state = ConnectionHandlingState::Pending;
            info.current_segment_index = -1;
            info.meta_dirty = true;
        }

        for (auto& seg : info.segments) {
            if (seg.state == ConnectionHandlingState::Processing) {
                seg.state = ConnectionHandlingState::Pending;
                seg.dirty = true;
            }
        }
    }

    return true;
}

bool DisconnectSessionManager::stopSegment(const std::string& stream_id, int32_t segment_index) {
    DisconnectSession* session = getProcessingSession();
    if (!session) return false;

    for (auto& info : session->stream_handle_infos) {
        if (info.stream_id != stream_id) continue;

        if (segment_index < 0 || segment_index >= static_cast<int32_t>(info.segments.size()))
            return false;

        auto& seg = info.segments[segment_index];

        if (seg.state != ConnectionHandlingState::Processing) {
            return false;
        }

        seg.state = ConnectionHandlingState::Pending;
        seg.dirty = true;

        sync();
        return true;
    }

    return false;
}

void DisconnectSessionManager::push(uint64_t disconnect_time, uint64_t reconnect_time) {
    DisconnectSession session;
    session.session_id = "sess-" + std::to_string(disconnect_time);
    session.disconnect_time = disconnect_time;
    session.reconnect_time = reconnect_time;
    session.session_state = ConnectionHandlingState::Initializing;
    session.progress_percent = 0.0;
    session.meta_dirty = true;

    _sessions.push_back(session);
    sortSessions();
}

void DisconnectSessionManager::sortSessions() {
    std::sort(_sessions.begin(), _sessions.end(),
        [](const DisconnectSession& a, const DisconnectSession& b) {
            return a.disconnect_time < b.disconnect_time;
        });
}

void DisconnectSessionManager::sortSegments(StreamHandleInfo& info) {
    std::sort(info.segments.begin(), info.segments.end(),
        [](const RecordingVideoSegment& a, const RecordingVideoSegment& b) {
            return a.start_time < b.start_time;
        });
}

double DisconnectSessionManager::calcStreamProgress(const StreamHandleInfo& info) const {
    if (info.segments.empty()) return 0.0;

    double total_dur = 0.0, weighted = 0.0;
    for (const auto& seg : info.segments) {
        double dur = static_cast<double>(seg.end_time - seg.start_time);
        double p = 0.0;
        if (seg.state == ConnectionHandlingState::Success) {
            p = 100.0;
        } else if (seg.processed_up_to > static_cast<uint64_t>(seg.start_time)) {
            p = std::min(100.0, 100.0 * static_cast<double>(seg.processed_up_to - seg.start_time) / dur);
        }
        total_dur += dur;
        weighted  += p * dur;
    }
    return total_dur > 0.0 ? weighted / total_dur : 0.0;
}

void DisconnectSessionManager::sync() {
    DisconnectSession* session = getProcessingSession();
    if (!session) return;

    for (auto& info : session->stream_handle_infos) {
        bool any_processing = false;
        bool any_pending = false;
        bool any_failed = false;
        bool all_done = true;

        for (const auto& seg : info.segments) {
            if (seg.state == ConnectionHandlingState::Processing) {
                any_processing = true;
                all_done = false;
            } else if (seg.state == ConnectionHandlingState::Pending) {
                any_pending = true;
                all_done = false;
            } else if (seg.state == ConnectionHandlingState::Failed) {
                any_failed = true;
            }
        }

        // Processing > Pending > (all_done â†’ Success/Failed)
        if (any_processing) {
            info.handle_state = ConnectionHandlingState::Processing;
        } else if (any_pending) {
            info.handle_state = ConnectionHandlingState::Pending;
        } else if (all_done) {
            info.handle_state = any_failed ? ConnectionHandlingState::Failed : ConnectionHandlingState::Success;
            info.current_segment_index = -1;
        }
        info.meta_dirty = true;
    }

    bool any_processing = false;
    bool any_pending = false;
    bool any_failed = false;
    bool all_done = true;

    for (const auto& info : session->stream_handle_infos) {
        if (info.handle_state == ConnectionHandlingState::Processing) {
            any_processing = true;
            all_done = false;
        } else if (info.handle_state == ConnectionHandlingState::Pending) {
            any_pending = true;
            all_done = false;
        } else if (info.handle_state == ConnectionHandlingState::Failed) {
            any_failed = true;
        }
    }

    if (any_processing) {
        session->session_state = ConnectionHandlingState::Processing;
    } else if (any_pending) {
        session->session_state = ConnectionHandlingState::Pending;
    } else if (all_done) {
        session->session_state = any_failed ? ConnectionHandlingState::Failed : ConnectionHandlingState::Success;
        if (session->processing_end_time == 0)
            session->processing_end_time = static_cast<uint64_t>(time(nullptr));
    }
    session->meta_dirty = true;
}

} // namespace managerkit