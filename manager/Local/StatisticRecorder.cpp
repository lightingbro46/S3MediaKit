#include "StatisticRecorder.h"
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(StatisticRecorder)

StatisticRecorder::StatisticRecorder(const string &record_path) : _record_path(record_path) {
    if (_record_path.empty()) {
        GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath)
        GET_CONFIG(string, app_name, Record::kAppName)
        _record_path = File::absolutePath(app_name, mp4_save_path);
    } 
}

StatisticRecorder::~StatisticRecorder() {
    std::lock_guard<std::mutex> lock(_mtx_stats);
    _cam_stats_map.clear();
}

CameraStatisticImp::Ptr StatisticRecorder::getRecorder(const string &device_id) {
    {
        std::lock_guard<std::mutex> lock(_mtx_stats);
        if (_cam_stats_map.find(device_id) != _cam_stats_map.end()) {
            return _cam_stats_map[device_id];
        }
    }
    return addRecorder(device_id);
}

CameraStatisticImp::Ptr StatisticRecorder::addRecorder(const string &device_id) {
    auto device_path = _record_path + "/" + device_id;
    auto imp = std::make_shared<CameraStatisticImp>(device_path);
    weak_ptr<StatisticRecorder> weak_self = shared_from_this();
    imp->setOnRemove([weak_self](const std::string &device_id) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->removeRecorder(device_id);
    });
    {
        std::lock_guard<std::mutex> lock(_mtx_stats);
        _cam_stats_map.emplace(device_id, imp);
    }
    return imp;
}

bool StatisticRecorder::removeRecorder(const string &device_id) {
    std::lock_guard<std::mutex> lock(_mtx_stats);
    if (_cam_stats_map.find(device_id) == _cam_stats_map.end()) {
        return false;
    }
    _cam_stats_map.erase(device_id);
    return true;
}

void StatisticRecorder::addArchiveSize(const string &device_id, const string &stream_id, size_t count, size_t size, uint64_t archive_start_time, uint64_t archive_end_time, bool add) {
    auto recorder = getRecorder(device_id);
    recorder->addArchiveSize(stream_id, count, size, archive_start_time, archive_end_time, add);
}

void StatisticRecorder::addBookmarkCount(const string &device_id, uint64_t created_at, bool add) {
    auto recorder = getRecorder(device_id);
    recorder->addBookmarkCount(created_at, 0, add);
}

void StatisticRecorder::addDeviceCapabilities(const string &device_id, bool connected, const string &status, const DeviceCapabilities *device_caps) {
    auto recorder = getRecorder(device_id);
    recorder->addDeviceCapabilities(connected, status, device_caps);
}

void StatisticRecorder::addStreamStatistic(const string &device_id, int stream_type, bool live, const string &status, const TranslationInfo *info) {
    auto recorder = getRecorder(device_id);
    recorder->addStreamStatistic(stream_type, live, status, info);
}

void StatisticRecorder::loadSavedCameraStatistics(const std::function<void(CameraStatisticImp::Ptr &stats)> &invoker) {
    File::scanDir(_record_path, [&](const string &path, bool isDir) {
        if (isDir) {
            auto saved_path = path + "/info.txt";
            if (File::fileExist(saved_path) && File::fileSize(saved_path) > 0) {
                DebugL << "Found saved file: " << saved_path << ". Loading...";
                auto imp = std::make_shared<CameraStatisticImp>(saved_path);
                weak_ptr<StatisticRecorder> weak_self = shared_from_this();
                imp->setOnRemove([weak_self](const string &device_id) {
                    auto strong_self = weak_self.lock();
                    if (!strong_self) {
                        return;
                    }
                    strong_self->removeRecorder(device_id);
                });
                auto stats = imp->getParams();
                {
                    std::lock_guard<std::mutex> lock(_mtx_stats);
                    _cam_stats_map.emplace(stats.tuple.device_id, imp);
                }
                invoker(imp);
                return true;
            }
            WarnL << "Saved file empty or invalid format: " << saved_path << ". Ignore";
            File::delete_file(saved_path, true);
        }
        return true;
    });
}

} // namespace managerkit