#ifndef CAMERA_STREAMQUALITY_H
#define CAMERA_STREAMQUALITY_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace managerkit {

struct StreamBaselineConfig {
    int warmup_sec = 120;      // live this long before samples count (encoder start, first GOP)
    int window_sec = 300;      // samples averaged into the baseline
    double max_cv = 0.15;      // a window whose fps stddev exceeds 15 % of its mean is dropped
    double drift_ratio = 0.2;  // fps off by more than 20 % ...
    int drift_relearn_sec = 3600; // ... for this long (and nothing else wrong): learn again
};

// Copy of one stream's quality counters at a point in time.
struct StreamQualitySnapshot {
    bool live = false;
    int64_t live_since = 0;          // unix seconds of the last live change
    int64_t window_started_at = 0;   // start of the uptime window (max(now - 24 h, tracking start))
    double uptime_pct_24h = -1;      // -1 until anything was observed
    int disconnects_1h = 0;
    int disconnects_24h = 0;
    // Last sample (taken with the stream statistics, every 60 s while live).
    double fps = -1;
    double bitrate_kbps = -1;
    int64_t sampled_at = 0;
    // Baseline learnt while the stream was steady; < 0 = not learnt yet.
    double fps_baseline = -1;
    double bitrate_baseline = -1;
    int64_t baseline_at = 0;
    double loss_pct = -1;            // RTP loss of the last check, -1 = not measured (non-RTP source)
    int64_t loss_at = 0;
};

/**
 * Quality counters of one camera stream for the health monitor. Written on the camera poller
 * (live changes, 60 s statistic samples) and the media poller (loss rate), read by the monitor:
 * every method locks a small mutex. The clock is passed in so tests drive it.
 */
class StreamQuality {
public:
    using Ptr = std::shared_ptr<StreamQuality>;

    explicit StreamQuality(int64_t now);

    void onLive(bool live, int64_t now);
    // A statistic sample of a live stream. fps <= 0 (not known yet at connect) is ignored.
    // `format` (codec + resolution) changing restarts the baseline.
    void onSample(double fps, double bitrate_kbps, const std::string &format, int64_t now,
                  const StreamBaselineConfig &cfg);
    void onLossRate(double loss_pct, int64_t now);
    // Learn the baseline again (camera stream settings changed from the VMS).
    void resetBaseline();

    StreamQualitySnapshot snapshot(int64_t now) const;

    // Pure baseline step, exposed for tests: mean of `fps` when the samples span `window_sec`
    // and their coefficient of variation is <= max_cv.
    static bool steadyMean(const std::vector<double> &values, double max_cv, double &mean);

private:
    void trim(int64_t now);

private:
    struct Sample {
        int64_t at;
        double fps;
        double bitrate_kbps;
    };

    mutable std::mutex _mtx;
    int64_t _tracking_since;
    bool _live = false;
    int64_t _live_since;
    // Live/not-live transitions of the last 24 h (bounded), oldest first.
    std::deque<std::pair<int64_t, bool> > _changes;
    bool _live_at_window_start = false; // state before the oldest kept transition
    std::deque<int64_t> _disconnects;

    double _fps = -1;
    double _bitrate_kbps = -1;
    int64_t _sampled_at = 0;
    std::string _format;
    std::deque<Sample> _window;      // candidate baseline samples
    double _fps_baseline = -1;
    double _bitrate_baseline = -1;
    int64_t _baseline_at = 0;
    int64_t _drift_since = 0;

    double _loss_pct = -1;
    int64_t _loss_at = 0;
};

} // namespace managerkit

#endif // CAMERA_STREAMQUALITY_H
