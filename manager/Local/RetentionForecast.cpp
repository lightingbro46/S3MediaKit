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
    val["committed_cameras"] = committed_cameras;
    val["committed_bytes"] = static_cast<Json::Int64>(committed_bytes);
    val["retention_days_estimate"] = daysJson(retention_days_estimate);
    val["estimated_remaining_days"] = daysJson(estimated_remaining_days);
    val["required_days"] = daysJson(required_days);
    val["committed_days"] = daysJson(committed_days);
    val["state"] = level > 0 ? "low" : "normal";
    val["level"] = level > 1 ? "critical" : (level > 0 ? "warning" : "normal");
    return val;
}

RetentionForecast computeRetentionForecast(const RetentionForecastInput &in, float safety_factor) {
    RetentionForecast out;
    out.valid = in.ingest_valid;
    out.ingest_mbps = std::max(0.0f, in.ingest_mbps);
    const int usable_percent = std::min(100, std::max(0, in.usable_percent));
    out.usable_bytes = std::max<int64_t>(0, in.total_bytes) / 100 * usable_percent;
    out.used_bytes = std::max<int64_t>(0, in.used_bytes);

    auto sum = [](const std::vector<RetentionCameraDemand> &cameras, int &count) {
        double bytes = 0;
        for (const auto &cam : cameras) {
            if (cam.required_sec <= 0 || cam.mbps <= 0.0f) {
                continue;
            }
            bytes += cam.mbps * kMiB * static_cast<double>(cam.required_sec);
            ++count;
        }
        return bytes;
    };
    out.required_bytes = static_cast<int64_t>(sum(in.cameras, out.cameras));
    out.committed_bytes = static_cast<int64_t>(sum(in.committed, out.committed_cameras));

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
    out.committed_days = out.committed_bytes / daily;
    const double factor = std::max(1.0f, safety_factor);
    auto isShort = [&](int64_t bytes) { return bytes > 0 && bytes * factor > static_cast<double>(out.usable_bytes); };
    out.observed = isShort(out.committed_bytes) ? std::max(1, in.short_level) : (isShort(out.required_bytes) ? 1 : 0);
    return out;
}

std::map<std::string, RetentionPoolDemand> planRetentionDemand(const std::vector<RetentionCameraPlan> &cameras,
                                                               const std::map<std::string, int64_t> &usable_bytes,
                                                               float safety_factor) {
    std::map<std::string, RetentionPoolDemand> out;
    for (const auto &cam : cameras) {
        const double bytes_per_sec = std::max(0.0f, cam.mbps) * kMiB;
        for (const auto &stay : cam.stays) {
            auto &pool = out[stay.pool_id];
            pool.inflow_mbps += std::max(0.0f, cam.mbps);
            if (stay.sec > 0 && bytes_per_sec > 0)
                pool.camera_bytes[cam.camera_id] += bytes_per_sec * static_cast<double>(stay.sec);
        }
    }

    const double factor = std::max(1.0f, safety_factor);
    // Every pool belongs to one tier and stays only go down the tiers, so a tier's spill is final
    // before the next tier is evaluated.
    for (int tier = 0; tier <= 2; ++tier) {
        for (auto &kv : out) {
            auto &pool = kv.second;
            bool in_tier = false;
            for (const auto &cam : cameras) {
                for (const auto &stay : cam.stays)
                    in_tier = in_tier || (stay.pool_id == kv.first && stay.tier == tier);
            }
            if (!in_tier)
                continue;
            pool.required_bytes = 0;
            for (const auto &c : pool.camera_bytes)
                pool.required_bytes += c.second;
            auto cap = usable_bytes.find(kv.first);
            if (cap == usable_bytes.end() || pool.required_bytes <= 0)
                continue;
            const double excess = pool.required_bytes - static_cast<double>(cap->second) / factor;
            if (excess <= 0)
                continue;
            pool.short_level = std::max(pool.short_level, 1);
            for (const auto &cam : cameras) {
                auto share = pool.camera_bytes.find(cam.camera_id);
                if (share == pool.camera_bytes.end())
                    continue;
                const double moved = excess * share->second / pool.required_bytes;
                for (size_t i = 0; i < cam.stays.size(); ++i) {
                    const auto &stay = cam.stays[i];
                    if (stay.pool_id != kv.first)
                        continue;
                    if (stay.spill && i + 1 < cam.stays.size()) {
                        out[cam.stays[i + 1].pool_id].camera_bytes[cam.camera_id] += moved;
                    } else {
                        pool.short_level = 2;
                        pool.overflow_action = stay.overflow_action;
                    }
                    break;
                }
            }
        }
    }
    for (auto &kv : out) {
        if (kv.second.overflow_action.empty())
            kv.second.overflow_action = "MOVE_TO_NEXT_TIER";
    }
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
