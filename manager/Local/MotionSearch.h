#ifndef LOCAL_MOTIONSEARCH_H
#define LOCAL_MOTIONSEARCH_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/MediaSource.h"
#include "Motion/MotionDemuxer.h"

namespace managerkit {

/**
 * A merged, clipped motion time range (seconds since epoch).
 * Mirrors TimeRange in TimeQuery so callers can use the same data types.
 */
struct MotionTimeRange {
    uint64_t startTime; // seconds since epoch
    uint32_t duration;  // seconds
};

/**
 * Queries motion recording files (.mblk / .idx) for a single stream.
 *
 * Files are discovered automatically from base_path (YYYYMMDD.mblk pattern).
 * All timestamps passed in / returned are seconds since the Unix epoch
 * — consistent with TimeQuery conventions. Internally, MotionInterval
 * stamps are in milliseconds and are converted on the fly.
 *
 * Thread safety: all public methods are protected by an internal mutex.
 */
class MotionSearch {
public:
    using Ptr = std::shared_ptr<MotionSearch>;

    /**
     * @param tuple      Stream identity (used to build base_path when omitted).
     * @param base_path  Directory that contains YYYYMMDD.mblk files.
     *                   When empty, defaults to {MP4SavePath}/motion/{app}/{stream}/
     */
    explicit MotionSearch(const mediakit::MediaTuple &tuple, const std::string &base_path = "");
    ~MotionSearch() = default;

    const mediakit::MediaTuple &getMediaTuple() const { return _tuple; }

    /**
     * Flat list of merged motion intervals in [start_time, end_time].
     * Consecutive/overlapping intervals are merged into one.
     * Mirrors: TimeQuery::getRecordedTimePeriod(cb<vector<TimeRange>>)
     */
    void getMotionTimePeriod(uint64_t start_time, uint64_t end_time,
        const std::function<void(std::vector<MotionTimeRange> &)> &cb);

    /**
     * Per-date set of hours that have at least one motion interval.
     * All date keys in [start_time, end_time] are pre-populated (empty set = no motion).
     * Mirrors: TimeQuery::getRecordedTimePeriod(cb<unordered_map<date, set<hour>>>)
     */
    void getMotionTimePeriod(uint64_t start_time, uint64_t end_time,
        const std::function<void(std::unordered_map<std::string /*date*/,
                                                    std::set<int /*hour*/>> &)> &cb);

    /**
     * Per-date, per-hour list of merged motion intervals (split at hour boundaries).
     * Mirrors: TimeQuery::getRecordedTimePeriod(cb<unordered_map<date, unordered_map<hour, vector<TimeRange>>>>)
     */
    void getMotionTimePeriod(uint64_t start_time, uint64_t end_time,
        const std::function<void(std::unordered_map<std::string /*date*/,
                                 std::unordered_map<int /*hour*/,
                                 std::vector<MotionTimeRange>>> &)> &cb);

private:
    /**
     * Core query: iterate all MotionIntervals in [start_ms, end_ms] and
     * invoke cb for each interval clipped to the query window.
     * start_ms / end_ms are milliseconds; cb receives seconds.
     */
    void query(uint64_t start_ms, uint64_t end_ms,
               const std::function<void(uint64_t iv_start_sec,
                                        uint64_t iv_end_sec)> &cb);

    /** Merge a [seg_start, seg_end] second-range into a flat result list. */
    static void mergeInto(std::vector<MotionTimeRange> &list,
                          uint64_t seg_start, uint64_t seg_end);

private:
    mediakit::MediaTuple              _tuple;
    std::string                       _base_path;
    mediakit::MultiMotionDemuxer::Ptr _demuxer;
    std::recursive_mutex              _mtx;
};

} // namespace managerkit

#endif // LOCAL_MOTIONSEARCH_H
