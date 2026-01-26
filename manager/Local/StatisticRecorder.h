#ifndef LOCAL_STATISTICRECORDER_H
#define LOCAL_STATISTICRECORDER_H

#include "Camera/CameraStatistic.h"

namespace managerkit {

class StatisticRecorder : public std::enable_shared_from_this<StatisticRecorder> {
public:
    using Ptr = std::shared_ptr<StatisticRecorder>;

    static StatisticRecorder &Instance();

    ~StatisticRecorder();

    void loadSavedCameraStatistics(const std::function<void(CameraStatisticImp::Ptr &stats)> &invoker);

    CameraStatisticImp::Ptr getRecorder(const std::string &device_id);

    bool removeRecorder(const std::string &device_id);

    void addArchiveSize(const std::string &device_id, const std::string &stream_id, size_t count, size_t size, uint64_t archive_start_time, uint64_t archive_end_time, bool add = true);

    void addBookmarkCount(const std::string &device_id, uint64_t created_at, bool add = true);

    void addDeviceCapabilities(const std::string &device_id, bool connected, const std::string &status, const DeviceCapabilities *device_caps = nullptr);

    void addStreamStatistic(const std::string &device_id, int stream_type, bool live, const std::string &status, const mediakit::TranslationInfo *info = nullptr);

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