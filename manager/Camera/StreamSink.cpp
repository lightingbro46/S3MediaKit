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
    new_cfg.protocol.enable_motion = option.enableMotion && option.motionDetectOnStream == type;
    new_cfg.protocol.roi_mask      = option.enableMotion ? option.roiValue : "";
    new_cfg.protocol.record_motion = option.enableMotion ? true : false;

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
                DebugL << (start ? "Start" : "Stop") << " record for primary stream of device " << _tuple.shortUrl() << " due to record mode is RecordOnlyMotion";
                it.second->setupRecord(Recorder::type_mp4, start);
            } else {
                // note: keep record secondary stream, even if record mode is RecordOnlyMotion, 
                // because secondary stream may be low resolution stream which is more likely to have motion detection enabled, and recording it does not consume much resource,
                // also it is easier to correlate motion events with video frames if both primary and secondary stream are recorded, 
                // so here we still record secondary stream if it is live, but the recording can be automatically deleted after certain period of time to save storage space
                DebugL << "Start record for secondary stream of device " << _tuple.shortUrl() << " due to record mode is RecordOnlyMotion";
                it.second->setupRecord(Recorder::type_mp4, true);
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
            // note: record primary stream when motion event start, and stop record when motion event stop,
            // and invert for secondary stream, record when motion event stop, and stop record when motion event start
            int start_record = it.first == StreamType::PrimaryStream ? start : !start;
            DebugL << (start_record ? "Start" : "Stop") << " record for stream type " << getStreamTypeString(it.first) << " of device " << _tuple.shortUrl() << " due to record mode is RecordLowResAndMotion";
            it.second->setupRecord(Recorder::type_mp4_archived, start_record);
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
        setupRecord(_archive_mode, true);
    }
}

} // namespace managerkit
