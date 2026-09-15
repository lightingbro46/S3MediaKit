#ifndef LOCAL_MOTIONSEARCH_H
#define LOCAL_MOTIONSEARCH_H

#ifdef ENABLE_MOTION

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <vector>
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
     * @param tuple          Stream identity (used to build base_path when omitted).
     * @param base_path      Directory that contains YYYY-MM-DD.mblk files.
     *                       When empty, defaults to {MP4SavePath}/motion/{app}/
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

    /**
     * Flat list of motion intervals filtered by ROI.
     * Only intervals where at least one ROI cell has a confirmed motion bit are returned.
     *
     * @param roi_mask  String of length rows*cols; each char '0' or '1'.
     *                  '0' = exclude cell, '1' = include cell (check for motion).
     *                  An empty string behaves like getMotionTimePeriod (no filter).
     */
    void getMotionTimePeriodByRoi(uint64_t start_time, uint64_t end_time,
        const std::string &roi_mask,
        const std::function<void(std::vector<MotionTimeRange> &)> &cb);

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

    /**
     * Returns true if the motion bitmap of the interval has at least one bit set
     * in a cell that is enabled in roi_mask (roi_mask[i] == '1').
     * Returns true unconditionally when roi_mask is empty or bitmap is empty.
     */
    static bool matchesRoi(const mediakit::MotionInterval &iv, const std::string &roi_mask);

private:
    mediakit::MediaTuple              _tuple;
    std::string                       _base_path;
    mediakit::MultiMotionDemuxer::Ptr _demuxer;
    std::recursive_mutex              _mtx;
};

namespace MotionRangeUtils {

inline uint64_t rangeEnd(const MotionTimeRange &range) {
    const uint64_t max_value = std::numeric_limits<uint64_t>::max();
    return range.duration > max_value - range.startTime
        ? max_value : range.startTime + range.duration;
}

inline void appendMerged(std::vector<MotionTimeRange> &ranges,
                         uint64_t start, uint64_t end) {
    if (start >= end) return;
    if (!ranges.empty()) {
        MotionTimeRange &last = ranges.back();
        const uint64_t last_end = rangeEnd(last);
        if (start <= last_end) {
            last.duration = static_cast<uint32_t>(std::max(last_end, end) - last.startTime);
            return;
        }
    }
    ranges.push_back({start, static_cast<uint32_t>(end - start)});
}

inline std::vector<MotionTimeRange> filterByRecordedRanges(
    const std::vector<MotionTimeRange> &motion_ranges,
    const std::vector<MotionTimeRange> &recorded_ranges) {
    std::vector<MotionTimeRange> motions = motion_ranges;
    std::vector<MotionTimeRange> recordings = recorded_ranges;
    const auto by_start = [](const MotionTimeRange &a, const MotionTimeRange &b) {
        return a.startTime < b.startTime;
    };
    std::sort(motions.begin(), motions.end(), by_start);
    std::sort(recordings.begin(), recordings.end(), by_start);

    std::vector<MotionTimeRange> normalized_recordings;
    for (const auto &range : recordings) {
        if (!range.duration) continue;
        appendMerged(normalized_recordings, range.startTime, rangeEnd(range));
    }

    std::vector<MotionTimeRange> result;
    size_t recording_index = 0;
    for (const auto &motion : motions) {
        if (!motion.duration) continue;
        const uint64_t motion_end = rangeEnd(motion);
        while (recording_index < normalized_recordings.size() &&
               rangeEnd(normalized_recordings[recording_index]) <= motion.startTime) {
            ++recording_index;
        }
        for (size_t i = recording_index; i < normalized_recordings.size(); ++i) {
            const auto &recording = normalized_recordings[i];
            if (recording.startTime >= motion_end) break;
            const uint64_t recording_end = rangeEnd(recording);
            const uint64_t intersection_start = std::max(motion.startTime, recording.startTime);
            const uint64_t intersection_end = std::min(motion_end, recording_end);
            appendMerged(result, intersection_start, intersection_end);
        }
    }
    return result;
}

} // namespace MotionRangeUtils

} // namespace managerkit

#endif // ENABLE_MOTION

#endif // LOCAL_MOTIONSEARCH_H
