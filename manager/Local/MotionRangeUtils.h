#ifndef LOCAL_MOTIONRANGEUTILS_H
#define LOCAL_MOTIONRANGEUTILS_H

#ifdef ENABLE_MOTION

#include "MotionSearch.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace managerkit {
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

#endif // LOCAL_MOTIONRANGEUTILS_H
