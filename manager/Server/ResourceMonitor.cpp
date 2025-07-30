#include "ResourceMonitor.h"
#include "Util/util.h"
#include <iomanip>

using namespace std;
using namespace toolkit;

namespace managerkit {

std::string format_double_2f(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value;
    return oss.str();
}

std::string format_bytes_human_readable(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double size = static_cast<double>(bytes);
    int unit_index = 0;

    while (size >= 1024.0 && unit_index < 5) {
        size /= 1024.0;
        ++unit_index;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    return oss.str();
}

string formatDuration(int64_t milliseconds) {
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

////////////////////////////////////ResourceMonitor//////////////////////////////////////////

string getResourceTypeString(const ResourceType &type) {
#define SWITCH_CASE(type) case ResourceType::type : return #type
    switch (type) {
        SWITCH_CASE(CPU);
        SWITCH_CASE(MEMORY);
        SWITCH_CASE(NETWORK);
        SWITCH_CASE(HDD);
        default : return "unknown";
    }
}

void ResourceMonitor::setThreshold(double warning_threshold, double critical_threshold) {
    _warning_threshold = warning_threshold;
    _critical_threshold = critical_threshold;
}

std::pair<double, double> ResourceMonitor::getThreshold() {
    return std::make_pair(_warning_threshold, _critical_threshold);
}

void ResourceMonitor::emitSystemAlert(double usage) {
    if (_critical_threshold < 0 && _critical_threshold < 0) {
        TraceL << "No system " << getResourceTypeString(_type) << " threshold config. Ignore system alert";
        return;
    }
    if (_critical_threshold > 0 && usage >= _critical_threshold) {
        auto flag = NOTICE_EMIT(BroadcastSystemAlertArgs, mediakit::Broadcast::kBroadcastSystemAlert, static_cast<uint8_t>(_type), usage, _critical_threshold, true);
        if (!flag) {
            TraceL << "No one listen system critical alert event";
        }
        return;
    }
    if (_warning_threshold > 0 && usage >= _warning_threshold) {
        auto flag = NOTICE_EMIT(BroadcastSystemAlertArgs, mediakit::Broadcast::kBroadcastSystemAlert, static_cast<uint8_t>(_type), usage, _warning_threshold, false);
        if (!flag) {
            TraceL << "No one listen system warning alert event";
        }
        return;
    }
    TraceL << "System " << getResourceTypeString(_type) << " usage is normal: " << format_double_2f(usage) << "%";
}

} // namespace managerkit
