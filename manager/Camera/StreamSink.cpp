#include "StreamSink.h"
#include "RecordPolicy.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {
    
StreamSink::StreamSink(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller) : _tuple(tuple), _poller(poller) {}

StreamSink::~StreamSink() {
    if (_poller && !_poller->isCurrentThread()) {
        WarnL << "StreamSink destroyed outside poller thread, cleanup may race with pending tasks";
    }
    _timer_sink.reset();
    _monitor_map.clear();
}

void StreamSink::setListener(const std::weak_ptr<DeviceSourceEvent> &listener) {
    setDelegate(listener);
}

void StreamSink::createTimer() {
    weak_ptr<StreamSink> weak_self = shared_from_this();
    _timer_sink = std::make_shared<Timer>(
        60.0f,
        [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            strong_self->onManager();
            return true;
        },
        _poller);
}

void StreamSink::setupMonitor(int type, const StreamTuple &tuple, const CameraOption &option) {
    if (!_poller->isCurrentThread()) {
        auto self = shared_from_this();
        _poller->async([self, type, tuple, option]() {
            self->setupMonitor(type, tuple, option);
        });
        return;
    }

    if (!isValidStreamType(type)) {
        WarnL << "Invalid stream type: " << type;
        return;
    }

    bool record_mp4 = option.enableRecord;
    if ((type == StreamType::PrimaryStream && option.doNotRecordPrimaryStream) || (type == StreamType::SecondaryStream && option.doNotRecordSecondaryStream)) {
        record_mp4 = false;
    }
    int media_port = option.autoMediaPort ? 0 : option.mediaPort;
    // todo: support rtp transport multicast mode
    int rtp_type = option.rtpTransport == option.kRtpTransportUdp ? 1 /*udp mode*/ : 0 /*tcp mode*/;

    StreamOption new_cfg;
    new_cfg.tuple                  = tuple;
    new_cfg.record_mp4             = record_mp4;
    new_cfg.rtp_type               = rtp_type;
    new_cfg.media_port             = media_port;
    new_cfg.username               = option.username;
    new_cfg.password               = option.password;
    new_cfg.protocol.enable_mp4    = false; // recording is controlled by StreamSource itself
    new_cfg.protocol.enable_audio  = !option.disableAudio;
    // GOP ring buffer: only primary needs it (pre-event backfill for startEventRecord).
    // Secondary uses setupRecord(type_mp4) for continuous recording which does not
    // require a ring reader, so enable_gop_cache=false saves memory on the secondary.
    new_cfg.protocol.enable_gop_cache = (type == StreamType::PrimaryStream);
    {
        GET_CONFIG(int, gop_cache_size, mediakit::Protocol::kGopCacheSize);
        new_cfg.protocol.gop_cache_size = gop_cache_size;
    }
    // Event-based recording window (only relevant for primary).
    new_cfg.protocol.pre_record_ms  = (type == StreamType::PrimaryStream)
                                      ? static_cast<uint32_t>(option.motionPreRecordSec  * 1000)
                                      : 0;
    new_cfg.protocol.post_record_ms = (type == StreamType::PrimaryStream)
                                      ? static_cast<uint32_t>(option.motionPostRecordSec * 1000)
                                      : 0;
#ifdef ENABLE_MOTION
    new_cfg.protocol.enable_motion = option.enableMotion && option.motionDetectOnStream == type;
    new_cfg.protocol.roi_mask      = option.enableMotion ? option.roiValue : "";
    new_cfg.protocol.record_motion = option.enableMotion ? true : false;
    // Tell MotionMuxer to listen for kBroadcastRecordMP4 from the primary stream.
    // Only the primary stream uses event_session_record triggered by motion start/end.
    // When motion detection runs on a non-primary stream, _primary_stream_id holds
    // the primary stream's ID (set earlier in setupMonitor for PrimaryStream).
    if (type == StreamType::PrimaryStream) {
        _primary_stream_id = tuple.stream_id;
    }
    if (new_cfg.protocol.enable_motion && type != StreamType::PrimaryStream) {
        new_cfg.protocol.motion_record_stream_id = _primary_stream_id;
    }
#else
    new_cfg.protocol.enable_motion = false;
    new_cfg.protocol.roi_mask      = "";
    new_cfg.protocol.record_motion = false;
#endif // ENABLE_MOTION

    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        if (it->second->getOption() == new_cfg) {
            DebugL << "Monitor for stream type " << getStreamTypeString(type) << " of device " << _tuple.shortUrl() << " already exists with same config, skip recreate";
            return;
        }
        _monitor_map.erase(type);
    }
    auto monitor = std::make_shared<StreamSource>(type, new_cfg);
    monitor->setListener(shared_from_this());
    monitor->start();
    _monitor_map.emplace(type, monitor);
}

void StreamSink::stopMonitor(int type) {
    if (!_poller->isCurrentThread()) {
        auto self = shared_from_this();
        _poller->async([self, type]() {
            self->stopMonitor(type);
        });
        return;
    }

    if (!isValidStreamType(type)) {
        return;
    }

    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        _monitor_map.erase(type);
    }
}

void StreamSink::onManager() {
    // Always called from the Timer which runs on _poller - no dispatch needed.
    for (const auto &it : _monitor_map) {
        auto type = it.first;
        if (!isValidStreamType(type)) {
            continue;
        }
        auto monitor = it.second;
        if (!monitor || !monitor->isLive()) 
            continue;
        // update stream statistic if stream is live, because only live stream has valid translation info
        auto status = monitor->getStatus();
        auto info = monitor->getTranslationInfo();
        auto data = Any::make<TranslationInfo>(std::move(info));
        onStreamReady(DeviceSource::NullDeviceSource(), type, true, status, data);
    }
}

bool StreamSink::setupRecord(int archive_mode, bool event_active) {
    // Callers: onRecordModeChange (asserts isCurrentThread) and setStreamRegist (already on poller).

    // ── Cleanup guard: handle state left by the previous mode before switching ──
    {
        bool is_same_mode = archive_mode == _archive_mode;
        bool was_event_mode = _archive_mode == static_cast<int>(RecordMode::RecordOnlyMotion)
                           || _archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion);
        bool now_event_mode = archive_mode == static_cast<int>(RecordMode::RecordOnlyMotion)
                           || archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion);
        if (was_event_mode && !now_event_mode) {
            // Cancel any active primary EventRecordSessions.  The session recorder
            // is NOT stored in muxer->_mp4 so the incoming branches cannot detect it
            // via isRecording(); without this, the orphaned session would keep writing.
            for (auto &it : _monitor_map) {
                if (it.second) {
                    it.second->cancelEventRecord();
                }
            }
        }

        // Stop any primary continuous (type_mp4) recorders left over from a previous mode (e.g. RecordAlways).
        bool was_record_primary = _archive_mode == static_cast<int>(RecordMode::RecordAlways);
        if (was_record_primary && !is_same_mode) {
            auto sec_it = _monitor_map.find(StreamType::PrimaryStream);
            if (sec_it != _monitor_map.end() && sec_it->second) {
                sec_it->second->setupRecord(Recorder::type_mp4, false);
            }
        }

        // Stop any secondary continuous (type_mp4) recorders left over from a previous mode (e.g. RecordAlways or RecordLowResAndMotion).
        // 
        bool was_record_secondary = _archive_mode == static_cast<int>(RecordMode::RecordAlways) 
                                    || _archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion);
        bool now_record_secondary = archive_mode == static_cast<int>(RecordMode::RecordAlways) 
                                    || archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion);
        if (was_record_secondary && !now_record_secondary) {
            auto sec_it = _monitor_map.find(StreamType::SecondaryStream);
            if (sec_it != _monitor_map.end() && sec_it->second) {
                sec_it->second->setupRecord(Recorder::type_mp4, false);
            }
        }
    }

    if (archive_mode == static_cast<int>(RecordMode::NoRecord)) {
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first] || !it.second->isLive())
                continue;
            // note: stop record both primary and secondary stream if record mode is RecordMode::NoRecord, even if record mode is RecordOnlyMotion or RecordLowResAndMotion,
            // because RecordMode::NoRecord means no recording at all, so we stop record for both primary and secondary stream to save resource, also it is easier to correlate motion events with video frames if both primary and secondary stream are recorded,
            // so we stop record for both stream when record mode is RecordMode::NoRecord, but the recording can be automatically deleted after certain period of time to save storage space
            DebugL << "Stop record for stream type " << getStreamTypeString(it.first) << " of device " << _tuple.shortUrl() << " due to record mode is NoRecord";
            it.second->setupRecord(Recorder::type_mp4, false);
        }
    } else if (archive_mode == static_cast<int>(RecordMode::RecordOnlyMotion)) {
#ifdef ENABLE_MOTION
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first] || !it.second->isLive())
                continue;

            if (it.first != StreamType::PrimaryStream) {
                // Secondary stream: no recording in RecordOnlyMotion.
                continue;
            }

            // Primary stream only: event-based recording via EventRecordSession.
            if (event_active) {
                if (it.second->hasActiveEventSession()) {
                    DebugL << "Extend active event record for primary stream of device " << _tuple.shortUrl()
                           << " due to overlapping motion event (RecordOnlyMotion)";
                    it.second->extendEventRecord();
                } else {
                    DebugL << "Start event record for primary stream of device " << _tuple.shortUrl()
                           << " due to RecordOnlyMotion";
                    it.second->startEventRecord();
                }
            } else {
                DebugL << "Stop event record for primary stream of device " << _tuple.shortUrl()
                       << " due to RecordOnlyMotion motion end";
                it.second->stopEventRecord(0);
            }
        }
#else
        WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
#endif // ENABLE_MOTION
    } else if (archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion)) {
#ifdef ENABLE_MOTION
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first] || !it.second->isLive())
                continue;

            if (it.first == StreamType::PrimaryStream) {
                // Primary: event-based clip with GOP pre-roll backfill.
                if (event_active) {
                    if (it.second->hasActiveEventSession()) {
                        DebugL << "Extend active event record for primary stream of device " << _tuple.shortUrl()
                               << " due to overlapping motion event";
                        it.second->extendEventRecord();
                    } else {
                        DebugL << "Start event record for primary stream of device " << _tuple.shortUrl()
                               << " due to RecordLowResAndMotion";
                        it.second->startEventRecord();
                    }
                } else {
                    // Motion ended: primary records post_record_ms tail then auto-closes.
                    DebugL << "Stop event record for primary stream of device " << _tuple.shortUrl()
                           << " due to RecordLowResAndMotion motion end";
                    it.second->stopEventRecord(0);
                }
            } else {
                // Secondary stream: records continuously at all,
                // Uses plain setupRecord(type_mp4) so no ring buffer is required.
                DebugL << "Start secondary stream record for device " << _tuple.shortUrl();
                it.second->setupRecord(Recorder::type_mp4, true);
            }
        }
#else
        WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
#endif // ENABLE_MOTION
    } else if (archive_mode == static_cast<int>(RecordMode::RecordAlways)) {
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first] || !it.second->isLive())
                continue;
            // note: always record both primary and secondary stream if they are live, even if record mode is RecordAlways
            DebugL << "Start record for stream type " << getStreamTypeString(it.first) << " of device " << _tuple.shortUrl() << " due to record mode is RecordAlways";
            it.second->setupRecord(Recorder::type_mp4, true);
        }
    } else {
        WarnL << "Unsupported record mode: " << getRecordModeString(static_cast<RecordMode>(archive_mode));
        return false;
    }
    _archive_mode = archive_mode;
    return true;
}

void StreamSink::onStreamReady(DeviceSource &sender, int type, bool live, const std::string &status, const toolkit::Any &data) {
    if (!_poller->isCurrentThread()) {
        auto self = shared_from_this();
        _poller->async([self, type, live, status, data]() {
            self->onStreamReady(DeviceSource::NullDeviceSource(), type, live, status, data);
        });
        return;
    }
    if (!isValidStreamType(type)) {
        WarnL << "Invalid stream type in onStreamReady: " << type;
        return;
    }

    DeviceSourceEventInterceptor::onStreamReady(sender, type, live, status, data);
}

void StreamSink::setStreamRegist(int type, bool regist, bool event_active) {
    // Caller (GenericRtspCameraImp::setupStreamRegist) asserts isCurrentThread - no dispatch needed.
    if (!isValidStreamType(type)) {
        WarnL << "Invalid stream type in setStreamRegist: " << type;
        return;
    }
    // Implement the logic to register or unregister the stream based on the 'regist' flag
    bool was_ready = _stream_ready[type];
    _stream_ready[type] = regist; // update before calling setupRecord so the guard inside sees the correct state
    if (!was_ready && regist) {
        // Pass the current event_active state so that a stream which registers
        // while a motion event is already in progress immediately starts recording.
        setupRecord(_archive_mode, event_active);
    }
}

} // namespace managerkit
