#include <algorithm>
#include <cmath>
#include "RetentionForecast.h"

using namespace std;

namespace managerkit {

static const double kMiB = 1024.0 * 1024.0;

static Json::Value daysJson(double days) {
    if (days < 0 || !std::isfinite(days)) {
        return Json::Value(Json::nullValue);
    }
    return Json::Value(std::round(days * 100.0) / 100.0);
}

Json::Value RetentionForecast::toJson() const {
    Json::Value val;
    val["ingest_mbps"] = valid ? Json::Value(sanitize_for_json(ingest_mbps)) : Json::Value(Json::nullValue);
    val["cameras"] = cameras;
    val["usable_bytes"] = static_cast<Json::Int64>(usable_bytes);
    val["used_bytes"] = static_cast<Json::Int64>(used_bytes);
    val["required_bytes"] = static_cast<Json::Int64>(required_bytes);
    val["retention_days_estimate"] = daysJson(retention_days_estimate);
    val["estimated_remaining_days"] = daysJson(estimated_remaining_days);
    val["required_days"] = daysJson(required_days);
    val["state"] = level > 0 ? "low" : "normal";
    return val;
}

RetentionForecast computeRetentionForecast(const RetentionForecastInput &in, float safety_factor) {
    RetentionForecast out;
    out.valid = in.ingest_valid;
    out.ingest_mbps = std::max(0.0f, in.ingest_mbps);
    const int usable_percent = std::min(100, std::max(0, in.usable_percent));
    out.usable_bytes = std::max<int64_t>(0, in.total_bytes) / 100 * usable_percent;
    out.used_bytes = std::max<int64_t>(0, in.used_bytes);

    double required = 0;
    for (const auto &cam : in.cameras) {
        if (cam.required_sec <= 0 || cam.mbps <= 0.0f) {
            continue;
        }
        required += cam.mbps * kMiB * static_cast<double>(cam.required_sec);
        ++out.cameras;
    }
    out.required_bytes = static_cast<int64_t>(required);

    if (!out.valid) {
        return out;
    }
    const double daily = out.ingest_mbps * kMiB * 86400.0;
    if (daily <= 0) {
        // Nothing records here: whatever is configured cannot run short.
        out.observed = 0;
        return out;
    }
    out.retention_days_estimate = out.usable_bytes / daily;
    out.estimated_remaining_days = std::max<int64_t>(0, out.usable_bytes - out.used_bytes) / daily;
    out.required_days = out.required_bytes / daily;
    const double factor = std::max(1.0f, safety_factor);
    out.observed = out.required_bytes > 0 && out.required_bytes * factor > static_cast<double>(out.usable_bytes)
        ? std::max(1, in.short_level)
        : 0;
    return out;
}

bool RetentionForecastBook::update(const std::string &key, RetentionForecast &forecast, int sustain,
                                   int64_t repeat_ms, int64_t now_ms) {
    lock_guard<mutex> lck(_mtx);
    auto &entry = _entries[key];
    bool emit = false;
    // Unknown keeps the previous state instead of reporting a recovery.
    if (forecast.observed >= 0) {
        emit = entry.alert.advance(forecast.observed, now_ms, std::max(1, sustain), repeat_ms);
    }
    forecast.level = entry.alert.level;
    entry.forecast = forecast;
    return emit;
}

bool RetentionForecastBook::get(const std::string &key, RetentionForecast &out) const {
    lock_guard<mutex> lck(_mtx);
    auto it = _entries.find(key);
    if (it == _entries.end()) {
        return false;
    }
    out = it->second.forecast;
    return true;
}

void RetentionForecastBook::remove(const std::string &key) {
    lock_guard<mutex> lck(_mtx);
    _entries.erase(key);
}

} // namespace managerkit
