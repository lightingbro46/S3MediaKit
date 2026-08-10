#include "ReaderMonitor.h"
#include "Common/macros.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

ReaderMonitor::~ReaderMonitor() {
    if (_alive_flag) _alive_flag->store(false, std::memory_order_release);
    _timer.reset();
}

ReaderCountInfoMap ReaderMonitor::getCurrentUsage() {
    lock_guard<mutex> lck(_mtx);
    return aggregateReaderCountsLocked();
}

int ReaderMonitor::totalReaderCount() {
    return _total_reader.load();
}

int ReaderMonitor::totalReaderCount(const std::string &camera_id) {
    auto reader_map = getCurrentUsage();
    auto it = reader_map.find(camera_id);
    if (it != reader_map.end()) {
        auto count = it->second;
        return count.first + count.second;
    }
    return 0;
}

static int totalReaderCountFromMap(const ReaderCountInfoMap &map_reader) {
    int ret = 0; 
    for (const auto &it : map_reader) {
        auto count = it.second;
        ret += count.first + count.second;
    }
    return ret;
}

unordered_map<std::string, int> ReaderMonitor::totalEachReaderCount() {
    unordered_map<std::string, int> ret;
    auto reader_map = getCurrentUsage();
    for (const auto &it : reader_map) {
        auto count = it.second;
        ret.emplace(it.first, count.first + count.second);
    }
    return ret;
}

ReaderCountInfoMap ReaderMonitor::aggregateReaderCountsLocked() const {
    ReaderCountInfoMap reader_map;
    for (const auto &entry : _source_reader) {
        auto &aggregate = reader_map[entry.second.camera_id];
        if (entry.first.find("|record:") != string::npos) {
            aggregate.second += entry.second.reader_count;
        } else {
            aggregate.first += entry.second.reader_count;
        }
    }
    return reader_map;
}

void ReaderMonitor::setStreamReaderCount(const string &camera_id, const string &source_id,
                                         int reader_count) {
    int current_count = 0;
    {
        lock_guard<mutex> lck(_mtx);
        auto source_it = _source_reader.find(source_id);
        if (source_it == _source_reader.end()) {
            // Do not create a permanent entry for a late zero-count event
            // (common when a replay source has already been removed).
            if (reader_count > 0) {
                source_it = _source_reader.emplace(source_id, SourceReaderCount()).first;
            }
        }
        if (source_it != _source_reader.end()) {
            auto &source = source_it->second;
            source.camera_id = camera_id;
            source.reader_count = reader_count;

            // A source with no live or replay readers must not remain in the map.
            // Replay URLs are timestamp-based and can otherwise grow this map forever.
            if (source.reader_count <= 0) {
                _source_reader.erase(source_it);
            }
        }

        auto reader_map = aggregateReaderCountsLocked();
        auto it = reader_map.find(camera_id);
        if (it != reader_map.end()) {
            auto count = it->second;
            current_count = count.first + count.second;
        }
        _total_reader = totalReaderCountFromMap(reader_map);
    }
    emitStreamReaderAlert(camera_id, current_count);
    emitSystemAlert(static_cast<float>(_total_reader.load()));
    // DebugL << "Set stream reader count for camera " << camera_id << ": " << current_count << ", total reader count: " << _total_reader.load();
}

void ReaderMonitor::start() {
    _alive_flag = std::make_shared<std::atomic<bool>>(true);
    auto alive = _alive_flag;
    _timer = std::make_shared<Timer>(300.0f, [this, alive]() {
        if (!alive->load(std::memory_order_acquire)) return false;
        // Periodically check and emit alerts
        emitSystemAlert(static_cast<float>(_total_reader.load()));

        // Check each camera reader count and emit alerts
        auto ret = totalEachReaderCount();
        for (const auto &it : ret) {
            emitStreamReaderAlert(it.first, it.second);
        }

        return true;
    }, _poller);
}

void ReaderMonitor::emitStreamReaderAlert(const string &camera_id, int usage_count) {
    if (_stream_reader_warning_threshold < 0 && _stream_reader_critical_threshold < 0) {
        TraceL << "No stream reader threshold config. Ignore stream reader alert";
        return;
    }
    if (_stream_reader_warning_threshold > 0 && usage_count >= _stream_reader_warning_threshold) {
        auto flag = NOTICE_EMIT(BroadcastStreamReaderAlertArgs, mediakit::Broadcast::kBroadcastStreamReaderAlert, camera_id, usage_count, _stream_reader_warning_threshold, true);
        if (!flag) {
            TraceL << "No one listen stream reader alert event";
        }
        return;
    }
    TraceL << "Stream reader " << camera_id << " usage is normal: " << usage_count << "/" << _stream_reader_warning_threshold;
}

void ReaderMonitor::setStreamReaderThreshold(int warning_threshold, int critical_threshold) {
    lock_guard<mutex> lck(_mtx);
    _stream_reader_warning_threshold = warning_threshold;
    _stream_reader_critical_threshold = critical_threshold;
}

bool ReaderMonitor::isReaderCountLimit(const string &camera_id, bool record) {
    auto total_count = totalReaderCount();
    if (_critical_threshold > 0 && total_count + 1 > _critical_threshold) {
        WarnL << "Total reader count has reached the critical threshold " << total_count << "/" << _critical_threshold << ". Ignore new reader for camera " << camera_id;
        return true;
    }

    auto reader_map = getCurrentUsage();
    auto it = reader_map.find(camera_id);
    if (it != reader_map.end()) {
        auto count = it->second;
        int current_count = count.first + count.second;
        lock_guard<mutex> lck(_mtx);
        if (_stream_reader_critical_threshold > 0 && current_count + 1 >= _stream_reader_critical_threshold) {
            WarnL << "Camera " << camera_id << " has reader count that reached the critical threshold " << current_count << "/"  << _stream_reader_critical_threshold << ". Ignore new reader";
            return true;
        }
    }
    
    return false;
}

bool ReaderMonitor::isReaderCountAvailable(const string &camera_id) {
    auto reader_map = getCurrentUsage();
    auto it = reader_map.find(camera_id);
    int current_count = 0;
    if (it != reader_map.end()) {
        auto count = it->second;
        current_count = count.first + count.second;
    }
    lock_guard<mutex> lck(_mtx);
    if (_stream_reader_warning_threshold > 0 && current_count + 1 >= _stream_reader_warning_threshold) {
        WarnL << "Camera " << camera_id << " has reader count that reached the warning threshold " << current_count << "/"  << _stream_reader_warning_threshold << ". Ignore new reader";
        return false;
    }
    return true;
}

bool ReaderMonitor::isReaderCountAvailable() {
    auto total_count = totalReaderCount();
    if (_warning_threshold > 0 && total_count + 1 > _warning_threshold) {
        WarnL << "Total reader count has reached the warning threshold " << total_count << "/" << _warning_threshold << ". Ignore new reader";
        return false;
    }
    return true;
}

} // namespace managerkit