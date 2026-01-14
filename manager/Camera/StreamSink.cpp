#include "StreamSink.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {
    
StreamSink::StreamSink(const toolkit::EventPoller::Ptr &poller) : _poller(poller) {}

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

void StreamSink::setupMonitor(int type, const StreamTuple &tuple, const CameraInfo &info, const CameraOption &option) {
    bool start_record = option.enableRecord;
    if ((type == PrimaryStream && option.doNotRecordPrimaryStream) || (type == SecondaryStream && option.doNotRecordSecondaryStream)) {
        start_record = false;
    }
    int rtp_type = option.rtpTransport == option.kRtpTransportUdp ? 1 /*udp mode*/ : 0 /*tcp mode*/;
    int media_port = option.autoMediaPort ? 0 :  option.mediaPort;

    StreamSource::Ptr monitor;
    {
        lock_guard<mutex> lck(_mtx_sink);
        auto it = _monitor_map.find(type);
        if (it != _monitor_map.end()) {
            if (start_record == it->second->isRecording() && rtp_type == it->second->getRtpType() && media_port == it->second->getMediaPort()) {
                TraceL << "Stream " << tuple.shortUrl() << " config do not change. Ignore";
                return;
            }
            _monitor_map.erase(type);
        }

        monitor = std::make_shared<StreamSource>(tuple, start_record, rtp_type, media_port, info.username, info.password);
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

} // namespace managerkit
