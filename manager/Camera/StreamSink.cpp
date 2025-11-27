#include "StreamSink.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {
    
StreamSink::StreamSink() {
    _timer_sink = std::make_shared<Timer>(
        60.0f,
        [&]() {
            onManager();
            return true;
        },
        nullptr);
}

StreamSink::~StreamSink() {
    _timer_sink.reset();
}

void StreamSink::setupMonitor(int type, const StreamTuple &tuple, bool start_record, int rtp_type, int media_port) {
    bool need_recreate = false;
    {
        lock_guard<recursive_mutex> lck(_mtx_sink);
        auto it = _monitor_map.find(type);
        if (it != _monitor_map.end()) {
            if (start_record == it->second->isRecording() && rtp_type == it->second->getRtpType() && media_port == it->second->getMediaPort()) {
                TraceL << "Stream " << tuple.shortUrl() << " config do not change. Ignore";
                return;
            }
            _monitor_map.erase(type);
        }
        need_recreate = true;
    }
    
    if (need_recreate) {
        // Create and configure monitor outside lock to avoid blocking other operations
        auto monitor = std::make_shared<StreamSource>(tuple, start_record, rtp_type, media_port);
        monitor->setOnStreamChange([type, this]() { 
            onStreamChange(type);
        });
        monitor->start();
        
        // Only update map under lock
        {
            lock_guard<recursive_mutex> lck(_mtx_sink);
            _monitor_map[type] = monitor;
        }
    }
}

void StreamSink::stopMonitor(int type) {
    lock_guard<recursive_mutex> lck(_mtx_sink);
    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        _monitor_map.erase(type);
    }
}

bool StreamSink::isStreamLive(int type) {
    lock_guard<recursive_mutex> lck(_mtx_sink);
    bool live = false;
    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        live = it->second->isLive();
    }
    return live;
}

string StreamSink::getStreamStatus(int type) {
    lock_guard<recursive_mutex> lck(_mtx_sink);
    string status = "no-monitor";
    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        status = it->second->getStatus();
    }
    return status;
}

TranslationInfo StreamSink::getStreamInfo(int type) {
    lock_guard<recursive_mutex> lck(_mtx_sink);
    TranslationInfo info;
    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        info = it->second->getTranslationInfo();
    }
    return info;
}

void StreamSink::onManager() {
    //todo:
    vector<int> live_streams;
    {
        lock_guard<recursive_mutex> lck(_mtx_sink);
        for (const auto &it : _monitor_map) {
            if (it.second->isLive()) {
                live_streams.push_back(it.first);
            }
        }
    }
    
    // Call virtual method outside lock to prevent deadlock
    for (int stream_type : live_streams) {
        onStreamChange(stream_type);
    }
}

} // namespace managerkit
