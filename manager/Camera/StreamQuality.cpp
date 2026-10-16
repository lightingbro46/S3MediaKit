#include "StreamQuality.h"
#include <algorithm>
#include <cmath>

using namespace std;

namespace managerkit {

static const int64_t kDaySec = 86400;
static const size_t kMaxChanges = 400;

StreamQuality::StreamQuality(int64_t now) : _tracking_since(now), _live_since(now) {}

void StreamQuality::trim(int64_t now) {
    while (!_disconnects.empty() && _disconnects.front() < now - kDaySec) {
        _disconnects.pop_front();
    }
    // A flapping stream must not grow the list without bound: the dropped changes only move the
    // known state at the start of the window.
    while (!_changes.empty() && (_changes.front().first < now - kDaySec || _changes.size() > kMaxChanges)) {
        _live_at_window_start = _changes.front().second;
        _changes.pop_front();
    }
}

void StreamQuality::onLive(bool live, int64_t now) {
    lock_guard<mutex> lck(_mtx);
    if (live == _live) {
        return;
    }
    if (_live && !live) {
        _disconnects.push_back(now);
        // The warm-up starts again after a reconnect; the baseline itself is kept. The last
        // sample described the old connection: no ratio until the new one is measured.
        _window.clear();
        _fps = -1;
        _bitrate_kbps = -1;
    }
    _changes.emplace_back(now, live);
    _live = live;
    _live_since = now;
    trim(now);
}

bool StreamQuality::steadyMean(const vector<double> &values, double max_cv, double &mean) {
    if (values.empty()) {
        return false;
    }
    double sum = 0;
    for (auto v : values) {
        sum += v;
    }
    mean = sum / values.size();
    if (mean <= 0) {
        return false;
    }
    double var = 0;
    for (auto v : values) {
        var += (v - mean) * (v - mean);
    }
    var /= values.size();
    return std::sqrt(var) <= max_cv * mean;
}

bool StreamQuality::restoreBaseline(const string &format, double fps, double bitrate_kbps, int64_t learnt_at) {
    lock_guard<mutex> lck(_mtx);
    if (_fps_baseline > 0 || fps <= 0 || format.empty() || (!_format.empty() && _format != format)) {
        return false;
    }
    // The first sample with another codec/resolution drops it again (onSample()).
    _format = format;
    _fps_baseline = fps;
    _bitrate_baseline = bitrate_kbps > 0 ? bitrate_kbps : -1;
    _baseline_at = learnt_at;
    _drift_since = 0;
    _window.clear();
    return true;
}

void StreamQuality::resetBaseline() {
    lock_guard<mutex> lck(_mtx);
    _window.clear();
    _fps_baseline = -1;
    _bitrate_baseline = -1;
    _baseline_at = 0;
    _drift_since = 0;
}

void StreamQuality::onSample(double fps, double bitrate_kbps, const string &format, int64_t now,
                             const StreamBaselineConfig &cfg) {
    lock_guard<mutex> lck(_mtx);
    if (fps <= 0) {
        return;
    }
    _fps = fps;
    _bitrate_kbps = bitrate_kbps;
    _sampled_at = now;
    if (!_format.empty() && format != _format) {
        // New codec or resolution: the old expectation no longer applies.
        _window.clear();
        _fps_baseline = -1;
        _bitrate_baseline = -1;
        _baseline_at = 0;
        _drift_since = 0;
    }
    _format = format;
    if (!_live || now - _live_since < cfg.warmup_sec) {
        return;
    }

    if (_fps_baseline > 0) {
        // The installer changed the camera's fps on purpose: after an hour at the new rate,
        // learn it instead of reporting it forever.
        if (std::fabs(fps - _fps_baseline) > cfg.drift_ratio * _fps_baseline) {
            if (!_drift_since) {
                _drift_since = now;
            } else if (now - _drift_since >= cfg.drift_relearn_sec) {
                _fps_baseline = -1;
                _bitrate_baseline = -1;
                _baseline_at = 0;
                _drift_since = 0;
                _window.clear();
            }
        } else {
            _drift_since = 0;
        }
        if (_fps_baseline > 0) {
            return;
        }
    }

    Sample s;
    s.at = now;
    s.fps = fps;
    s.bitrate_kbps = bitrate_kbps;
    _window.push_back(s);
    while (_window.size() > 1 && _window.back().at - _window.front().at > cfg.window_sec) {
        _window.pop_front();
    }
    // Samples come every ~60 s: a window is complete once they span most of window_sec.
    if (_window.size() < 3 || _window.back().at - _window.front().at < cfg.window_sec * 4 / 5) {
        return;
    }
    vector<double> fps_values;
    double bitrate_sum = 0;
    int bitrate_count = 0;
    for (const auto &w : _window) {
        fps_values.push_back(w.fps);
        if (w.bitrate_kbps > 0) {
            bitrate_sum += w.bitrate_kbps;
            ++bitrate_count;
        }
    }
    double mean = 0;
    if (steadyMean(fps_values, cfg.max_cv, mean)) {
        _fps_baseline = mean;
        _bitrate_baseline = bitrate_count ? bitrate_sum / bitrate_count : -1;
        _baseline_at = now;
        _window.clear();
    } else {
        // Unsteady window: slide and try with the next sample.
        _window.pop_front();
    }
}

void StreamQuality::onLossRate(double loss_pct, int64_t now) {
    lock_guard<mutex> lck(_mtx);
    _loss_pct = loss_pct;
    _loss_at = now;
}

StreamQualitySnapshot StreamQuality::snapshot(int64_t now) const {
    lock_guard<mutex> lck(_mtx);
    StreamQualitySnapshot out;
    out.live = _live;
    out.live_since = _live_since;
    for (auto t : _disconnects) {
        out.disconnects_24h += t >= now - kDaySec ? 1 : 0;
        out.disconnects_1h += t >= now - 3600 ? 1 : 0;
    }

    const int64_t start = std::max(now - kDaySec, _tracking_since);
    out.window_started_at = start;
    bool state = _live_at_window_start;
    int64_t t = start;
    int64_t live_time = 0;
    for (const auto &c : _changes) {
        if (c.first <= start) {
            state = c.second;
            continue;
        }
        if (state) {
            live_time += c.first - t;
        }
        t = c.first;
        state = c.second;
    }
    if (state && now > t) {
        live_time += now - t;
    }
    if (now > start) {
        out.uptime_pct_24h = 100.0 * (double)live_time / (double)(now - start);
    } else {
        out.uptime_pct_24h = _live ? 100.0 : -1;
    }

    out.fps = _fps;
    out.bitrate_kbps = _bitrate_kbps;
    out.sampled_at = _sampled_at;
    out.fps_baseline = _fps_baseline;
    out.bitrate_baseline = _bitrate_baseline;
    out.baseline_at = _baseline_at;
    out.format = _format;
    out.loss_pct = _loss_pct;
    out.loss_at = _loss_at;
    return out;
}

} // namespace managerkit
