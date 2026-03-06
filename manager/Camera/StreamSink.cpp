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

    ProtocolOption _option;
    _option.enable_mp4 = false; // do not enable mp4 in player proxy, recording is controlled by StreamSource itself
    _option.enable_audio = !option.disableAudio;
    _option.enable_motion = option.enableMotion && option.motionDetectOnStream == type;
    _option.roi_mask = option.enableMotion ? option.roiValue : "";
    _option.record_motion =  option.enableMotion ? true : false;

    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        // todo: check if the new option is different from current option, if not, skip recreate monitor
        _monitor_map.erase(type);
    }
    auto monitor = std::make_shared<StreamSource>(type, tuple, _option, record_mp4, rtp_type, media_port, option.username, option.password);
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
    if (!_poller->isCurrentThread()) {
        auto self = shared_from_this();
        _poller->async([self]() {
            self->onManager();
        });
        return;
    }
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
    if (!_poller->isCurrentThread()) {
        auto self = shared_from_this();
        _poller->async([self, archive_mode, start]() {
            self->setupRecord(archive_mode, start);
        });
        return true;
    }

    if (archive_mode == static_cast<int>(RecordMode::NoRecord)) {
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first]) 
                continue;

            it.second->setupRecord(Recorder::type_mp4, false);
        }
    } else if (archive_mode == static_cast<int>(RecordMode::RecordOnlyMotion)) {
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first]) 
                continue;

            if (it.first == StreamType::PrimaryStream) {
                it.second->setupRecord(Recorder::type_mp4, start);
            } else {
                it.second->setupRecord(Recorder::type_mp4, false);
            }
        }
    } else if (archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion)) {
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first]) 
                continue; 

            int record_ = it.first == StreamType::PrimaryStream ? start : !start;
            it.second->setupRecord(Recorder::type_mp4_archived, record_);
        }
    } else if (archive_mode == static_cast<int>(RecordMode::RecordAlways)) {
        for (auto &it : _monitor_map) {
            if (!_stream_ready[it.first]) 
                continue;

            it.second->setupRecord(Recorder::type_mp4, true);
        }
    } else {
        WarnL << "Unsupported record mode: " << archive_mode;
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

    if (_stream_ready[type] != live && live) {
        setupRecord(_archive_mode, true);
    }
    // update stream status
    _stream_ready[type] = live;
    DeviceSourceEventInterceptor::onStreamReady(sender, type, live, status, data);
}

} // namespace managerkit
