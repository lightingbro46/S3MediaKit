#ifndef LOCAL_STATISTICRECORDER_H
#define LOCAL_STATISTICRECORDER_H

#include "Camera/CameraStatistic.h"

namespace managerkit {

/**
 * Normalized time range returned by StatisticRecorder::normalize*TimeRange().
 * start/end are seconds since epoch, clamped to the stored archive bounds.
 * isValid() returns false when start >= end (no data in the requested window).
 */
struct QueryTimeRange {
    uint64_t start = 0;
    uint64_t end   = 0;
    bool isValid() const { return start < end; }
};

class StatisticRecorder : public std::enable_shared_from_this<StatisticRecorder> {
public:
    using Ptr = std::shared_ptr<StatisticRecorder>;

    static StatisticRecorder &Instance();

    ~StatisticRecorder();

    void loadSavedCameraStatistics(const std::function<void(CameraStatisticImp::Ptr &stats)> &invoker);

    CameraStatisticImp::Ptr getRecorder(const std::string &device_id, bool create_if_not_exist = true);

    bool removeRecorder(const std::string &device_id);

    void addArchiveSize(const std::string &device_id, const std::string &stream_id, size_t count, size_t size, uint64_t archive_start_time, uint64_t archive_end_time, bool add = true);

    void addBookmarkCount(const std::string &device_id, uint64_t created_at, bool add = true);

    void addDeviceCapabilities(const std::string &device_id, bool connected, const std::string &status, const DeviceCapabilities *device_caps = nullptr);

    void addStreamStatistic(const std::string &device_id, int stream_type, bool live, const std::string &status, const mediakit::TranslationInfo *info = nullptr);

    void addMotionKeepThreshold(const std::string &device_id, bool start, uint64_t threshold);

    void addTierKeepThreshold(const std::string &device_id, int tier_type, bool start, uint64_t threshold);

    /**
     * Clamp [start_time, end_time] to the archive bounds stored in StatisticRecorder
     * for a specific device/stream. Pass an empty stream_id to use the union of all streams.
     * Returns the input range unchanged when no statistic data is available.
     */
    QueryTimeRange normalizeArchiveTimeRange(const std::string &device_id, const std::string &stream_id, uint64_t start_time, uint64_t end_time);

    /**
     * Return per-stream clamped [start, end] ranges for all streams of a device.
     * Streams that have no statistic data use the raw [start_time, end_time] window.
     * Used by TimeQuery when querying across all streams so each stream is filtered
     * against its own archive bounds rather than the union.
     */
    std::unordered_map<std::string, QueryTimeRange> getArchiveTimeRangesPerStream(const std::string &device_id, uint64_t start_time, uint64_t end_time);

    /**
     * Clamp [start_time, end_time] to the motion archive bounds stored in StatisticRecorder
     * for a specific device.
     * Returns the input range unchanged when no statistic data is available.
     */
    QueryTimeRange normalizeMotionTimeRange(const std::string &device_id, uint64_t start_time, uint64_t end_time);

private:
    StatisticRecorder(const std::string &record_path = "");

    CameraStatisticImp::Ptr addRecorder(const std::string &device_id);

private:
    std::mutex _mtx_stats;
    std::string _record_path;
    std::unordered_map<std::string, CameraStatisticImp::Ptr> _cam_stats_map;
};

} // namespace managerkit

#endif // LOCAL_STATISTICRECORDER_H