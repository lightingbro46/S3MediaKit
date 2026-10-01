#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "Util/NoticeCenter.h"
#include "Util/util.h"
#include "ResourceMonitor.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

string format_double_2f(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value;
    return oss.str();
}

string format_float_2f(float value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value;
    return oss.str();
}

string format_bytes_human_readable(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    float size = static_cast<float>(bytes);
    int unit_index = 0;

    while (size >= 1024.0f && unit_index < 5) {
        size /= 1024.0f;
        ++unit_index;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    return oss.str();
}

string format_duration_verbose(int64_t milliseconds) {
    int64_t ms = milliseconds % 1000;
    int64_t total_seconds = milliseconds / 1000;
    int64_t seconds = total_seconds % 60;
    int64_t total_minutes = total_seconds / 60;
    int64_t minutes = total_minutes % 60;
    int64_t hours = total_minutes / 60;

    _StrPrinter oss;
    if (hours > 0) oss << hours << "h ";
    if (minutes > 0 || hours > 0) oss << minutes << "m ";
    if (seconds > 0 || minutes > 0 || hours > 0) oss << seconds << "s ";
    oss << ms << "ms";

    return oss;
}

string sanitize_for_json(double val) {
    double ret = val;
    if (std::isnan(val) || std::isinf(val))
        ret = 0.0;
    if (val <= 0.0) ret = 0.0; // chuyển -0.0 → +0.0
    return format_double_2f(ret);
}

string sanitize_for_json(float val) {
    float ret = val;
    if (std::isnan(val) || std::isinf(val))
        ret = 0.0f;
    if (val <= 0.0f) ret = 0.0f; // chuyển -0.0 → +0.0
    return format_float_2f(ret);
}

////////////////////////////////////ResourceMonitor//////////////////////////////////////////

string getResourceTypeString(const ResourceType &type) {
#define SWITCH_CASE(type) case ResourceType::type : return #type
    switch (type) {
        SWITCH_CASE(CPU);
        SWITCH_CASE(MEMORY);
        SWITCH_CASE(NETWORK);
        SWITCH_CASE(HDD);
        SWITCH_CASE(READER);
        SWITCH_CASE(DISK_IO);
        default : return "unknown";
    }
}

void ResourceMonitor::setThreshold(float warning_threshold, float critical_threshold) {
    _warning_threshold = warning_threshold;
    _critical_threshold = critical_threshold;
}

std::pair<float, float> ResourceMonitor::getThreshold() {
    return std::make_pair(_warning_threshold, _critical_threshold);
}

void ResourceMonitor::setAlertPolicy(int sustain_samples, int repeat_sec) {
    _alert_sustain_samples = std::max(1, sustain_samples);
    _alert_repeat_sec = repeat_sec;
}

void ResourceMonitor::setAlertClock(const std::function<int64_t()> &now_ms) {
    lock_guard<mutex> lck(_alert_mtx);
    _alert_clock = now_ms;
}

int64_t ResourceMonitor::alertNowMs() const {
    return _alert_clock ? _alert_clock() : steadyNowMs();
}

void ResourceMonitor::emitSystemAlert(float usage) {
    const float warning = _warning_threshold;
    const float critical = _critical_threshold;
    if (critical < 0 && warning < 0) {
        TraceL << "No system " << getResourceTypeString(_type) << " threshold config. Ignore system alert";
        return;
    }
    int observed = 0;
    if (critical > 0 && usage >= critical) {
        observed = 2;
    } else if (warning > 0 && usage >= warning) {
        observed = 1;
    }

    // Serialized: the reader count is reported from many threads.
    lock_guard<mutex> lck(_alert_mtx);
    const int previous = _alert.level;
    const int repeat_sec = _alert_repeat_sec;
    if (!_alert.advance(observed, alertNowMs(), _alert_sustain_samples, static_cast<int64_t>(repeat_sec) * 1000)) {
        return;
    }
    uint8_t type = static_cast<uint8_t>(_type);
    if (_alert.level == 0) {
        // Against the threshold of the alert it closes.
        const float threshold = previous == 2 ? critical : warning;
        InfoL << "System " << getResourceTypeString(_type) << " usage recovered: " << format_float_2f(usage);
        if (!NOTICE_EMIT(BroadcastSystemAlertRecoveredArgs, mediakit::Broadcast::kBroadcastSystemAlertRecovered, type, usage, threshold)) {
            TraceL << "No one listen system alert recovered event";
        }
        return;
    }
    bool is_critical = _alert.level == 2;
    float threshold = is_critical ? critical : warning;
    if (!NOTICE_EMIT(BroadcastSystemAlertArgs, mediakit::Broadcast::kBroadcastSystemAlert, type, usage, threshold, is_critical)) {
        TraceL << "No one listen system " << (is_critical ? "critical" : "warning") << " alert event";
    }
}

////////////////////////////////////AlertLevelTracker//////////////////////////////////////////

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool AlertLevelTracker::advance(int observed, int64_t now_ms, int sustain, int64_t repeat_ms) {
    const int need = std::max(1, sustain);
    if (observed > level) {
        _down_count = 0;
        _up_level = _up_count == 0 ? observed : std::min(_up_level, observed);
        if (++_up_count >= need) {
            level = _up_level;
            _up_count = 0;
            last_emit_ms = now_ms;
            return true;
        }
        return false;
    }
    if (observed < level) {
        _up_count = 0;
        _down_level = _down_count == 0 ? observed : std::max(_down_level, observed);
        if (++_down_count >= need) {
            level = _down_level;
            _down_count = 0;
            last_emit_ms = now_ms;
            return true;
        }
        return false;
    }
    _up_count = 0;
    _down_count = 0;
    if (level != 0 && repeat_ms > 0 && now_ms - last_emit_ms >= repeat_ms) {
        last_emit_ms = now_ms;
        return true;
    }
    return false;
}

} // namespace managerkit
