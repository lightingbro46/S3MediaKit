#include "StreamSink.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {
    
StreamSink::StreamSink(const std::unordered_map<int, StreamTuple> &stream_map) {
    for (const auto& it: stream_map) {
        _stream_map.emplace(it.first, make_pair(it.second, false));
    }
}

bool StreamSink::onStreamReady(int type) {
    lock_guard<recursive_mutex> lck(_mtx_sink);
    auto it = _stream_map.find(type);
    if (it != _stream_map.end()) {
        if (!it->second.second) {
            // set stream ready
            it->second.second = true;

            // check if all stream ready
            checkStreamIfReady();
        }
    }
    return true;
}

void StreamSink::checkStreamIfReady() {
    bool all_stream_ready = true;
    for (auto &ptr : _stream_map) {
        if (!ptr.second.second) {
            all_stream_ready = false;
        }
    }
    if (all_stream_ready) {
        emitAllStreamReady();
    }
}

void StreamSink::emitAllStreamReady() {
    if (_all_stream_ready) {
        return;
    }
    onAllStreamReady();
    _all_stream_ready = true;
}

void StreamSink::setupMonitor(int type, bool start_record, int rtp_type, int media_port) {
    lock_guard<recursive_mutex> lck(_mtx_sink);
    auto tuple = getStreamTuple(type);
    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        if (start_record == it->second->isRecording() && rtp_type == it->second->getRtpType() && media_port == it->second->getMediaPort()) {
            TraceL << "Stream " << tuple.shortUrl() << " config do not change. Ignore";
            return;
        }
        _monitor_map.erase(type);
    }
    auto monitor = std::make_shared<StreamSource>(tuple, start_record, rtp_type);
    monitor->setOnStreamReady([type, this]() { 
        onStreamReady(type); 
    });
    monitor->start();
    _monitor_map[type] = monitor;
}

void StreamSink::stopMonitor(int type) {
    lock_guard<recursive_mutex> lck(_mtx_sink);
    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        _monitor_map.erase(type);
    }

    onStreamReady(type);
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
    string status;
    auto it = _monitor_map.find(type);
    if (it != _monitor_map.end()) {
        status = it->second->getStatus();
    }
    return status;
}

void StreamSink::setupScheduler(const std::string &schedule_str) {
    auto _tmp_str = schedule_str;
    if (_tmp_str.empty()) {
        // input empty, set default value;
        string s(168, RecordModeHelper::toChar(RecordMode::RecordAlways));
        _tmp_str = s;
    }
    // compare config and recreate if config change
    if (_scheduler && _scheduler->getSchedulerString() == _tmp_str) {
        return;
    }

    _scheduler = std::make_shared<TimeScheduler<RecordMode, RecordModeHelper>>(_tmp_str);
    _scheduler->setOnChangeMode([&](RecordMode &mode) {
        if (_record_mode != mode) {
            DebugL << "Record mode change from " << RecordModeHelper::toString(_record_mode) << " to " << RecordModeHelper::toString(mode);
            _record_mode = mode;
            onChangeRecordMode(mode);
        }
    });
    _scheduler->start();
}

RecordMode StreamSink::getRecordModeActive() {
    lock_guard<recursive_mutex> lck(_mtx_sink);
    return _scheduler->getModeActive();
}

} // namespace managerkit
