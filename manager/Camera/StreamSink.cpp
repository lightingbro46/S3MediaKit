#include "StreamSink.h"
#include "RecordPolicy.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {
    
StreamSink::StreamSink(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller) : _tuple(tuple), _poller(poller) {}

StreamSink::~StreamSink() {
    lock_guard<mutex> lck(_mtx_sink);
    _timer_sink.reset();
    _monitor_map.clear();
}

void StreamSink::start() {
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
    bool record_mp4 = option.enableRecord;
    if ((type == StreamType::PrimaryStream && option.doNotRecordPrimaryStream) || (type == StreamType::SecondaryStream && option.doNotRecordSecondaryStream)) {
        record_mp4 = false;
    }
    // todo: support rtp transport multicast mode
    int rtp_type = option.rtpTransport == option.kRtpTransportUdp ? 1 /*udp mode*/ : 0 /*tcp mode*/;
    int media_port = option.autoMediaPort ? 0 :  option.mediaPort;

    ProtocolOption _option;
    _option.enable_mp4 = false; // do not enable mp4 in player proxy, recording is controlled by StreamSource itself
    _option.enable_audio = !option.disableAudio;
    _option.enable_motion = option.enableMotion && option.motionDetectOnStream == type;
    _option.roi_mask = option.enableMotion ? option.roiValue : "";
    _option.record_motion =  option.enableMotion ? true : false;

    StreamSource::Ptr monitor;
    {
        lock_guard<mutex> lck(_mtx_sink);
        auto it = _monitor_map.find(type);
        if (it != _monitor_map.end()) {
            // todo: check if the new option is different from current option, if not, skip recreate monitor
            _monitor_map.erase(type);
        }
        monitor = std::make_shared<StreamSource>(tuple, _option, record_mp4, rtp_type, media_port, option.username, option.password);
        _monitor_map.emplace(type, monitor);
    }
    
    weak_ptr<StreamSink> weak_self = shared_from_this();
    monitor->setOnStreamUpdate([type, weak_self](bool live, const std::string &status, const mediakit::TranslationInfo *info) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        if (strong_self->_on_stream_update) {
            strong_self->_on_stream_update(type, live, status, info);
        }
    });
    monitor->start();
}

void StreamSink::stopMonitor(int type) {
    lock_guard<mutex> lck(_mtx_sink);
    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        _monitor_map.erase(type);
    }
}

void StreamSink::onManager() {
    unordered_map<int, StreamSource::Ptr> monitor_list;
    {
        lock_guard<mutex> lck(_mtx_sink);
        for (const auto &it : _monitor_map) {
            if (it.second->isLive()) {
                monitor_list.emplace(it.first, it.second);
            }
        }
    }
    
    for (const auto &it : monitor_list) {
        auto type = it.first;
        auto monitor = it.second;
        // update stream statistic if stream is live, because only live stream has valid translation info
        if (_on_stream_update) {
            auto live = monitor->isLive();
            auto status = monitor->getStatus();
            auto info = monitor->getTranslationInfo();
            _on_stream_update(type, live, status, &info);
        }
    }
}

bool StreamSink::setupRecord(int archive_mode, bool start) {
    lock_guard<mutex> lck(_mtx_sink);
    if (archive_mode == static_cast<int>(RecordMode::NoRecord)) {
        for (auto &it : _monitor_map) {
            it.second->setupRecord(Recorder::type_mp4, false);
        }
    } else if (archive_mode == static_cast<int>(RecordMode::RecordOnlyMotion)) {
        for (auto &it : _monitor_map) {
            if (it.first == StreamType::PrimaryStream) {
                it.second->setupRecord(Recorder::type_mp4, start);
            } else {
                it.second->setupRecord(Recorder::type_mp4, false);
            }
        }
    } else if (archive_mode == static_cast<int>(RecordMode::RecordLowResAndMotion)) {
        for (auto &it : _monitor_map) {
            int record_ = it.first == StreamType::PrimaryStream ? start : !start;
            it.second->setupRecord(Recorder::type_mp4_archived, record_);
        }
    } else if (archive_mode == static_cast<int>(RecordMode::RecordAlways)) {
        for (auto &it : _monitor_map) {
            it.second->setupRecord(Recorder::type_mp4, true);
        }
    } else {
        WarnL << "Unsupported record mode: " << archive_mode;
        return false;
    }
    return true;
}

} // namespace managerkit
