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
    // Pre-create GOP ring buffer on both streams.
    // Primary: needed for pre-event backfill (pre_record_ms history).
    // Secondary: needed so startEventRecord() can attach a ring reader for live
    //   frame delivery (RecordLowResAndMotion rest-state continuous recording).
    //   Secondary is low-res, so a size-1 ring has negligible memory cost.
    new_cfg.protocol.enable_gop_cache     = true;
    // Primary: use global gop_cache_size config; secondary: size-1 is enough since
    // it only needs the ring for live frame delivery (no backfill).
    {
        GET_CONFIG(int, gop_cache_size, mediakit::Protocol::kGopCacheSize);
        new_cfg.protocol.gop_cache_size = (type == StreamType::PrimaryStream)
                                          ? gop_cache_size
                                          : 1;
    }
    // Event-based recording window.
    // Secondary has no pre-roll (no backfill desired; recording starts at next IDR).
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

bool StreamSink::setupRecord(int archive_mode, bool start) {
    // Callers: onRecordModeChange (asserts isCurrentThread) and setStreamRegist (already on poller).

    // ── Step 1: Always cancel any in-flight delayed task first. ─────────────
    // The task (pending secondary stream restart from a RecordLowResAndMotion
    // event end) must be cancelled unconditionally, not only inside the
    // RecordLowResAndMotion branch.  If the mode is changing away from
    // RecordLowResAndMotion the task must not fire after the switch.
    // cancel() must be called explicitly — dropping the shared_ptr only removes
    // our reference but the poller's _delay_task_map still holds its own copy.
    if (_switch_delay_task) {
        _switch_delay_task->cancel();
        _switch_delay_task = nullptr;
    }

    // ── Step 2: Cancel any active EventRecordSessions when leaving (or ────────
    // cross-switching between) event-based modes (RecordOnlyMotion and
    // RecordLowResAndMotion).  Both modes drive the primary stream via
    // startEventRecord().  The session recorder is NOT stored in muxer->_mp4,
    // so the mode-specific branches below cannot detect it via isRecording().
    // Without this:
    //   - exiting to NoRecord/RecordAlways: orphaned session keeps writing.
    //   - switching between the two event modes: old session overlaps with the
    //     new session created by the incoming motion event.
    {
        bool was_event_mode = _archive_mode == static_cast<int>(RecordMode::RecordOnlyMotion)
                           || _archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion);
        bool is_same_event_mode = archive_mode == _archive_mode;
        if (was_event_mode && !is_same_event_mode) {
            for (auto &it : _monitor_map) {
                if (it.second) {
                    it.second->cancelEventRecord();
                }
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

            if (it.first == StreamType::PrimaryStream) {
                // Use EventRecordSession (same as RecordLowResAndMotion) so that:
                //   - pre_record_ms history is backfilled (GOP cache on primary).
                //   - overlapping motion events extend the clip cleanly instead
                //     of restarting a new _mp4 recorder each time.
                //   - cancelEventRecord() in the cleanup guard above handles all
                //     mode-exit cleanup uniformly for both event-based modes.
                if (start) {
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
                    // Motion ended: record post_record_ms tail then close.
                    // No secondary-overlap delay needed — secondary records continuously.
                    DebugL << "Stop event record for primary stream of device " << _tuple.shortUrl()
                           << " due to RecordOnlyMotion motion end";
                    it.second->stopEventRecord(0);
                }
            } else {
                // Secondary (low-res): always-on continuous recording.
                // Recording it does not consume much resource and it correlates
                // motion events with video frames from both streams.
                DebugL << "Start record for secondary stream of device " << _tuple.shortUrl() << " due to RecordOnlyMotion";
                it.second->setupRecord(Recorder::type_mp4, true);
            }
        }
#else
        WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
#endif // ENABLE_MOTION
    } else if (archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion)) {
#ifdef ENABLE_MOTION
        // Stop any continuous (type_mp4) recorders left over from a previous mode
        // (e.g. RecordAlways). Without this they would keep recording in parallel
        // with the event-based / archived recorders managed by this mode.
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first] || !it.second->isLive())
                continue;
            it.second->setupRecord(Recorder::type_mp4, false);
        }

        // post_record_ms from primary defines the event tail duration.
        uint32_t post_ms = 0;
        {
            auto primary_it = _monitor_map.find(StreamType::PrimaryStream);
            if (primary_it != _monitor_map.end()) {
                post_ms = primary_it->second->getOption().protocol.post_record_ms;
            }
        }

        // secondary_gop_ms: overlap added to primary's post-event tail so primary is still
        // recording when secondary receives its first IDR after resuming.
        // Only computed for the !start (event-ends) path where it is actually consumed.
        uint32_t secondary_gop_ms = 0;
        if (!start) {
            GET_CONFIG(uint32_t, default_overlap_sec, Motion::kDefaultOverlapInterval);
            secondary_gop_ms = default_overlap_sec * 1000;
            auto secondary_it = _monitor_map.find(StreamType::SecondaryStream);
            if (secondary_it != _monitor_map.end()) {
                auto measured = secondary_it->second->getVideoGopIntervalMs();
                if (measured > 0) {
                    secondary_gop_ms = measured;
                    DebugL << "Secondary GOP interval for device " << _tuple.shortUrl()
                           << ": " << secondary_gop_ms << " ms (measured)";
                } else {
                    DebugL << "Secondary GOP interval not yet measured for device " << _tuple.shortUrl()
                           << ", using fallback " << secondary_gop_ms << " ms";
                }
            }
        }

        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first] || !it.second->isLive())
                continue;

            if (it.first == StreamType::PrimaryStream) {
                if (start) {
                    // Event begins: start primary event clip with GOP backfill.
                    // If a session is already active (overlapping event), extend it.
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
                    // Event ends: primary records (post_ms + secondary_gop_ms) then auto-closes.
                    // The extra secondary_gop_ms overlap ensures primary is still writing
                    // when secondary receives its first IDR.
                    DebugL << "Stopping event record for primary stream of device " << _tuple.shortUrl()
                           << " (post_ms=" << post_ms << " + overlap=" << secondary_gop_ms << " ms)";
                    it.second->stopEventRecord(secondary_gop_ms);
                }
            } else {
                // Secondary stream: inverse of primary — records at rest, pauses during event.
                // Uses EventRecordSession (startEventRecord with back_ms=0) so that secondary
                // goes through the same cleanup path as primary (cancelEventRecord in the guard),
                // and type_mp4_archived is no longer needed in muxer::setupRecord at all.
                if (start) {
                    // Event begins: cancel the active secondary session immediately so only
                    // primary covers the event window.
                    DebugL << "Stop secondary stream record for device " << _tuple.shortUrl()
                           << " (primary event recording started)";
                    it.second->cancelEventRecord();
                } else {
                    // Event ends: resume secondary after post_ms (while primary is still in
                    // the overlap window).  back_ms=0 (pre_record_ms=0 for secondary) so
                    // recording starts at the next IDR, avoiding overlap with primary's tail.
                    DebugL << "Scheduling secondary stream start for device " << _tuple.shortUrl()
                           << " after " << post_ms << " ms";
                    auto monitor = it.second;
                    _switch_delay_task = _poller->doDelayTask(post_ms, [monitor]() {
                        // Cancel any leftover session (e.g. stream reconnected before the
                        // previous session's ring detach callback fired) before starting fresh.
                        monitor->cancelEventRecord();
                        monitor->startEventRecord(); // back_ms=0, infinite session
                        return static_cast<uint64_t>(0); // run once
                    });
                }
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

void StreamSink::setStreamRegist(int type, bool regist) {
    // Caller (GenericRtspCameraImp::setupStreamRegist) asserts isCurrentThread - no dispatch needed.
    if (!isValidStreamType(type)) {
        WarnL << "Invalid stream type in setStreamRegist: " << type;
        return;
    }
    // Implement the logic to register or unregister the stream based on the 'regist' flag
    bool was_ready = _stream_ready[type];
    _stream_ready[type] = regist; // update before calling setupRecord so the guard inside sees the correct state
    if (!was_ready && regist) {
        // Use start=false to initialize to the rest state:
        //   RecordAlways        → start param unused (always starts type_mp4)
        //   RecordOnlyMotion    → primary waits for motion, secondary records
        //   RecordLowResAndMotion → primary waits for motion, secondary records
        //   NoRecord            → start param unused (always stops)
        setupRecord(_archive_mode, false);
    }
}

} // namespace managerkit
