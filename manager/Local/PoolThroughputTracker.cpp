#include <algorithm>
#include <iterator>
#include "PoolThroughputTracker.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

static const int64_t kBucketMs = 10 * 1000;
// Demand needs a few recorded segments per camera before it means anything.
static const int64_t kMinDemandObserveMs = 15 * 60 * 1000;
static const double kMiB = 1024.0 * 1024.0;

Json::Value PoolThroughputStats::toJson() const {
    Json::Value val;
    val["write_mbps"] = write_valid ? Json::Value(sanitize_for_json(write_mbps)) : Json::Value(Json::nullValue);
    val["write_busy_pct"] = sanitize_for_json(write_busy_pct);
    val["write_bytes"] = static_cast<Json::UInt64>(write_bytes);
    val["read_mbps"] = read_valid ? Json::Value(sanitize_for_json(read_mbps)) : Json::Value(Json::nullValue);
    val["read_busy_pct"] = sanitize_for_json(read_busy_pct);
    val["read_bytes"] = static_cast<Json::UInt64>(read_bytes);
    val["record_ingest_mbps"] = sanitize_for_json(record_ingest_mbps);
    val["avg_write_mbps"] = sanitize_for_json(avg_write_mbps);
    val["avg_read_mbps"] = sanitize_for_json(avg_read_mbps);
    val["demand_mbps"] = demand_valid ? Json::Value(sanitize_for_json(demand_mbps)) : Json::Value(Json::nullValue);
    val["demand_cameras"] = demand_cameras;
    val["backlog_bytes"] = static_cast<Json::Int64>(backlog_bytes);
    val["backlog_eta_sec"] = backlog_eta_sec >= 0 ? Json::Value(static_cast<Json::Int64>(backlog_eta_sec))
                                                  : Json::Value(Json::nullValue);
    val["state"] = low ? "low" : "normal";
    return val;
}

////////////////////////////////////Meter//////////////////////////////////////////

PoolThroughputTracker::Bucket &PoolThroughputTracker::Meter::bucketAt(int64_t at_ms) {
    const int64_t start = at_ms - at_ms % kBucketMs;
    if (buckets.empty() || buckets.back().start_ms < start) {
        Bucket b;
        b.start_ms = start;
        buckets.push_back(b);
        return buckets.back();
    }
    for (auto it = buckets.rbegin(); it != buckets.rend(); ++it) {
        if (it->start_ms <= start) {
            return *it;
        }
    }
    // Older than every bucket: the clock went backwards or the bucket was pruned; keep the oldest.
    return buckets.front();
}

void PoolThroughputTracker::Meter::creditBusy(int64_t now_ms) {
    if (active <= 0 || now_ms <= busy_since_ms) {
        return;
    }
    int64_t from = busy_since_ms;
    while (from < now_ms) {
        const int64_t bucket_end = from - from % kBucketMs + kBucketMs;
        const int64_t to = std::min(bucket_end, now_ms);
        bucketAt(from).busy_ms += to - from;
        from = to;
    }
    busy_since_ms = now_ms;
}

void PoolThroughputTracker::Meter::prune(int64_t oldest_ms) {
    while (!buckets.empty() && buckets.front().start_ms + kBucketMs <= oldest_ms) {
        buckets.pop_front();
    }
}

void PoolThroughputTracker::Meter::sum(int64_t oldest_ms, uint64_t &bytes, int64_t &busy_ms) const {
    bytes = 0;
    busy_ms = 0;
    for (const auto &b : buckets) {
        if (b.start_ms + kBucketMs <= oldest_ms) {
            continue;
        }
        bytes += b.bytes;
        busy_ms += b.busy_ms;
    }
}

////////////////////////////////////PoolThroughputTracker//////////////////////////////////////////

PoolThroughputTracker::PoolState &PoolThroughputTracker::pool(const std::string &pool_id) {
    return _pools[pool_id];
}

void PoolThroughputTracker::beginTransfer(const std::string &pool_id, Direction dir, int64_t now_ms) {
    if (pool_id.empty()) {
        return;
    }
    lock_guard<mutex> lck(_mtx);
    if (_started_ms < 0) {
        _started_ms = now_ms;
    }
    auto &st = pool(pool_id);
    auto &meter = dir == WRITE ? st.write : st.read;
    meter.creditBusy(now_ms);
    if (meter.active <= 0) {
        meter.active = 0;
        meter.busy_since_ms = now_ms;
    }
    ++meter.active;
}

void PoolThroughputTracker::endTransfer(const std::string &pool_id, Direction dir, uint64_t bytes, int64_t now_ms) {
    if (pool_id.empty()) {
        return;
    }
    lock_guard<mutex> lck(_mtx);
    auto &st = pool(pool_id);
    auto &meter = dir == WRITE ? st.write : st.read;
    meter.creditBusy(now_ms);
    if (meter.active > 0) {
        --meter.active;
    }
    if (bytes) {
        meter.bucketAt(now_ms).bytes += bytes;
    }
}

void PoolThroughputTracker::addIngest(const std::string &camera_id, const std::string &hot_pool_id,
                                      uint64_t bytes, int64_t now_ms) {
    if (camera_id.empty() || !bytes) {
        return;
    }
    lock_guard<mutex> lck(_mtx);
    if (_started_ms < 0) {
        _started_ms = now_ms;
    }
    _cameras[camera_id].bucketAt(now_ms).bytes += bytes;
    if (!hot_pool_id.empty()) {
        auto &st = pool(hot_pool_id);
        st.ingest.bucketAt(now_ms).bytes += bytes;
        st.camera_ingest[camera_id].bucketAt(now_ms).bytes += bytes;
    }
}

void PoolThroughputTracker::pruneCameras(int64_t oldest_ms) {
    for (auto it = _cameras.begin(); it != _cameras.end();) {
        it->second.prune(oldest_ms);
        if (it->second.buckets.empty()) {
            it = _cameras.erase(it);
        } else {
            ++it;
        }
    }
}

PoolIngestRates PoolThroughputTracker::ingestRates(const std::string &pool_id, int window_sec, int64_t now_ms) {
    PoolIngestRates out;
    const int64_t window_ms = static_cast<int64_t>(std::max(60, window_sec)) * 1000;
    const int64_t oldest_ms = now_ms - window_ms;

    lock_guard<mutex> lck(_mtx);
    // Nothing else prunes the camera meters where evaluate() never runs (tier storage off).
    pruneCameras(oldest_ms);
    const int64_t observed_ms = _started_ms < 0 ? 0 : std::min(window_ms, now_ms - _started_ms);
    out.valid = observed_ms >= std::min(window_ms, kMinDemandObserveMs);
    auto it = _pools.find(pool_id);
    if (it == _pools.end() || observed_ms <= 0) {
        return out;
    }
    const double observed_sec = observed_ms / 1000.0;
    auto &st = it->second;
    int64_t busy = 0;
    st.ingest.prune(oldest_ms);
    st.ingest.sum(oldest_ms, out.bytes, busy);
    out.mbps = static_cast<float>(out.bytes / kMiB / observed_sec);
    for (auto cam = st.camera_ingest.begin(); cam != st.camera_ingest.end();) {
        cam->second.prune(oldest_ms);
        if (cam->second.buckets.empty()) {
            cam = st.camera_ingest.erase(cam);
            continue;
        }
        uint64_t bytes = 0;
        cam->second.sum(oldest_ms, bytes, busy);
        out.camera_mbps[cam->first] = static_cast<float>(bytes / kMiB / observed_sec);
        ++cam;
    }
    return out;
}

void PoolThroughputTracker::setRoutes(const std::map<std::string, std::set<std::string>> &routes) {
    lock_guard<mutex> lck(_mtx);
    _routes = routes;
}

std::vector<PoolThroughputAlert> PoolThroughputTracker::evaluate(const std::map<std::string, int64_t> &backlog,
                                                                 const PoolThroughputConfig &cfg,
                                                                 int64_t now_ms) {
    std::vector<PoolThroughputAlert> alerts;
    const int64_t window_ms = static_cast<int64_t>(std::max(60, cfg.window_sec)) * 1000;
    const int64_t oldest_ms = now_ms - window_ms;

    lock_guard<mutex> lck(_mtx);
    const int64_t observed_ms = _started_ms < 0 ? 0 : std::min(window_ms, now_ms - _started_ms);
    const double observed_sec = observed_ms / 1000.0;
    const bool demand_known = observed_ms >= std::min(window_ms, kMinDemandObserveMs);

    // Recording rate per camera over the window.
    std::map<std::string, double> camera_mbps;
    pruneCameras(oldest_ms);
    for (const auto &kv : _cameras) {
        uint64_t bytes = 0;
        int64_t busy = 0;
        kv.second.sum(oldest_ms, bytes, busy);
        camera_mbps[kv.first] = observed_sec > 0 ? bytes / kMiB / observed_sec : 0.0;
    }

    std::set<std::string> pool_ids;
    for (const auto &kv : _pools) {
        pool_ids.insert(kv.first);
    }
    for (const auto &kv : _routes) {
        pool_ids.insert(kv.first);
    }
    for (const auto &kv : backlog) {
        pool_ids.insert(kv.first);
    }

    std::map<std::string, PoolThroughputStats> stats_map;
    for (const auto &pool_id : pool_ids) {
        auto &st = pool(pool_id);
        PoolThroughputStats s;
        s.pool_id = pool_id;

        st.write.creditBusy(now_ms);
        st.read.creditBusy(now_ms);
        st.write.prune(oldest_ms);
        st.read.prune(oldest_ms);
        st.ingest.prune(oldest_ms);
        for (auto cam = st.camera_ingest.begin(); cam != st.camera_ingest.end();) {
            cam->second.prune(oldest_ms);
            cam = cam->second.buckets.empty() ? st.camera_ingest.erase(cam) : std::next(cam);
        }

        int64_t write_busy = 0, read_busy = 0, ingest_busy = 0;
        uint64_t ingest_bytes = 0;
        st.write.sum(oldest_ms, s.write_bytes, write_busy);
        st.read.sum(oldest_ms, s.read_bytes, read_busy);
        st.ingest.sum(oldest_ms, ingest_bytes, ingest_busy);
        const int64_t min_busy_ms = static_cast<int64_t>(std::max(1, cfg.min_busy_sec)) * 1000;
        s.write_valid = write_busy >= min_busy_ms && s.write_bytes > 0;
        s.read_valid = read_busy >= min_busy_ms && s.read_bytes > 0;
        if (write_busy > 0) {
            s.write_mbps = static_cast<float>(s.write_bytes / kMiB / (write_busy / 1000.0));
        }
        if (read_busy > 0) {
            s.read_mbps = static_cast<float>(s.read_bytes / kMiB / (read_busy / 1000.0));
        }
        if (observed_ms > 0) {
            s.write_busy_pct = static_cast<float>(std::min(100.0, 100.0 * write_busy / observed_ms));
            s.read_busy_pct = static_cast<float>(std::min(100.0, 100.0 * read_busy / observed_ms));
            s.record_ingest_mbps = static_cast<float>(ingest_bytes / kMiB / observed_sec);
            s.avg_write_mbps = static_cast<float>((ingest_bytes + s.write_bytes) / kMiB / observed_sec);
            s.avg_read_mbps = static_cast<float>(s.read_bytes / kMiB / observed_sec);
        }

        auto route = _routes.find(pool_id);
        if (route != _routes.end()) {
            s.demand_valid = demand_known;
            s.demand_cameras = static_cast<int>(route->second.size());
            double demand = 0.0;
            for (const auto &camera_id : route->second) {
                auto rate = camera_mbps.find(camera_id);
                if (rate != camera_mbps.end()) {
                    demand += rate->second;
                }
            }
            s.demand_mbps = static_cast<float>(demand);
        }

        auto queued = backlog.find(pool_id);
        s.backlog_bytes = queued == backlog.end() ? 0 : std::max<int64_t>(0, queued->second);
        if (s.write_valid && s.write_mbps > 0) {
            s.backlog_eta_sec = static_cast<int64_t>(s.backlog_bytes / kMiB / s.write_mbps);
        }

        // Unknown stays unknown: no demand yet, or a backlog with no transfer to measure (pool
        // offline, dispatch blocked) keeps the previous state instead of reporting a recovery.
        int observed = -1;
        const float required = s.demand_mbps * std::max(1.0f, cfg.safety_factor);
        if (s.demand_valid) {
            if (s.backlog_bytes <= 0 || s.demand_mbps <= 0.0f) {
                observed = 0;
            } else if (s.write_valid) {
                observed = s.write_mbps < required ? 1 : 0;
            }
        }
        if (observed >= 0 && st.alert.advance(observed, now_ms, cfg.sustain_samples,
                                              static_cast<int64_t>(cfg.alert_repeat_sec) * 1000)) {
            PoolThroughputAlert alert;
            alert.required_mbps = required;
            alert.stats = s;
            alert.stats.low = st.alert.level > 0;
            alerts.push_back(alert);
        }
        s.low = st.alert.level > 0;
        stats_map[pool_id] = s;
    }

    // Forget pools nothing refers to any more.
    for (auto it = _pools.begin(); it != _pools.end();) {
        const auto &st = it->second;
        const bool idle = st.write.buckets.empty() && st.read.buckets.empty() && st.ingest.buckets.empty() &&
                          st.camera_ingest.empty() &&
                          st.write.active == 0 && st.read.active == 0 && st.alert.level == 0;
        if (idle && !_routes.count(it->first) && !backlog.count(it->first)) {
            stats_map.erase(it->first);
            it = _pools.erase(it);
        } else {
            ++it;
        }
    }
    _stats.swap(stats_map);
    return alerts;
}

bool PoolThroughputTracker::getStats(const std::string &pool_id, PoolThroughputStats &out) const {
    lock_guard<mutex> lck(_mtx);
    auto it = _stats.find(pool_id);
    if (it == _stats.end()) {
        return false;
    }
    out = it->second;
    return true;
}

void PoolThroughputTracker::removePool(const std::string &pool_id) {
    lock_guard<mutex> lck(_mtx);
    auto it = _pools.find(pool_id);
    // A transfer still running ends later and recreates an idle entry that the next evaluate() drops.
    if (it != _pools.end()) {
        _pools.erase(it);
    }
    _stats.erase(pool_id);
    _routes.erase(pool_id);
}

////////////////////////////////////PoolTransferScope//////////////////////////////////////////

PoolTransferScope::PoolTransferScope(PoolThroughputTracker &tracker, const std::string &write_pool,
                                     const std::string &read_pool)
    : _tracker(tracker), _write_pool(write_pool), _read_pool(read_pool) {
    const int64_t now = steadyNowMs();
    _tracker.beginTransfer(_write_pool, PoolThroughputTracker::WRITE, now);
    _tracker.beginTransfer(_read_pool, PoolThroughputTracker::READ, now);
}

PoolTransferScope::~PoolTransferScope() {
    finish(0);
}

void PoolTransferScope::finish(uint64_t bytes) {
    if (_done) {
        return;
    }
    _done = true;
    const int64_t now = steadyNowMs();
    _tracker.endTransfer(_write_pool, PoolThroughputTracker::WRITE, bytes, now);
    _tracker.endTransfer(_read_pool, PoolThroughputTracker::READ, bytes, now);
}

} // namespace managerkit
