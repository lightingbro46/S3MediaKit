#include "StatisticRecorder.h"
#include "Common/config.h"

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
    if (_speaker_path.empty()) {
        GET_CONFIG(string, speaker_save_path, Speaker::kSpeakerSavePath)
        GET_CONFIG(string, speaker_dir, Speaker::kSpeakerDir)
        _speaker_path = File::absolutePath(speaker_dir, speaker_save_path);
    }
}

StatisticRecorder::~StatisticRecorder() {
    std::lock_guard<std::mutex> lock(_mtx_stats);
    _cam_stats_map.clear();
    _sp_stats_map.clear();
}

CameraStatisticImp::Ptr StatisticRecorder::getRecorder(const string &device_id, bool create_if_not_exist) {
    CHECK(!device_id.empty());
    {
        std::lock_guard<std::mutex> lock(_mtx_stats);
        if (_cam_stats_map.find(device_id) != _cam_stats_map.end()) {
            return _cam_stats_map[device_id];
        }
    }
    return create_if_not_exist ? addRecorder(device_id) : nullptr;
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
        if (_cam_stats_map.find(device_id) != _cam_stats_map.end()) {
            _cam_stats_map.erase(device_id);
        }
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

SpeakerStatisticImp::Ptr StatisticRecorder::getSpeakerRecorder(const string &device_id, bool create_if_not_exist) {
    CHECK(!device_id.empty());
    {
        std::lock_guard<std::mutex> lock(_mtx_stats);
        if (_sp_stats_map.find(device_id) != _sp_stats_map.end()) {
            return _sp_stats_map[device_id];
        }
    }
    return create_if_not_exist ? addSpeakerRecorder(device_id) : nullptr;
}

SpeakerStatisticImp::Ptr StatisticRecorder::addSpeakerRecorder(const string &device_id) {
    auto device_path = _speaker_path + "/" + device_id;
    auto imp = std::make_shared<SpeakerStatisticImp>(device_path);
    weak_ptr<StatisticRecorder> weak_self = shared_from_this();
    imp->setOnRemove([weak_self](const std::string &device_id) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->removeSpeakerRecorder(device_id);
    });
    {
        std::lock_guard<std::mutex> lock(_mtx_stats);
        if (_sp_stats_map.find(device_id) != _sp_stats_map.end()) {
            _sp_stats_map.erase(device_id);
        }
        _sp_stats_map.emplace(device_id, imp);
    }
    return imp;
}

bool StatisticRecorder::removeSpeakerRecorder(const string &device_id) {
    std::lock_guard<std::mutex> lock(_mtx_stats);
    if (_sp_stats_map.find(device_id) == _sp_stats_map.end()) {
        return false;
    }
    _sp_stats_map.erase(device_id);
    return true;
}

void StatisticRecorder::addArchiveSize(const string &device_id, const string &stream_id, size_t count, size_t size, uint64_t archive_start_time, uint64_t archive_end_time, bool add) {
    auto recorder = getRecorder(device_id, false);
    if (!recorder) {
        WarnL << "Received archive size update for device " << device_id << " stream "<< stream_id << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addArchiveSize(stream_id, count, size, archive_start_time, archive_end_time, add);
}

void StatisticRecorder::addBookmarkCount(const string &device_id, uint64_t created_at, bool add) {
    auto recorder = getRecorder(device_id, false);
    if (!recorder) {
        WarnL << "Received bookmark count update for device " << device_id << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addBookmarkCount(created_at, 0, add);
}

void StatisticRecorder::addDeviceCapabilities(const string &device_id, bool connected, const string &status, const DeviceCapabilities *device_caps) {
    auto recorder = getRecorder(device_id, false);
    if (!recorder) {
        WarnL << "Received device capabilities update for device " << device_id << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addDeviceCapabilities(connected, status, device_caps);
}

void StatisticRecorder::addStreamStatistic(const string &device_id, int stream_type, bool live, const string &status, const TranslationInfo *info) {
    auto recorder = getRecorder(device_id, false);
    if (!recorder) {
        WarnL << "Received stream statistic update for device " << device_id << " stream type " << stream_type << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addStreamStatistic(stream_type, live, status, info);
}

void StatisticRecorder::loadSavedCameraStatistics(const std::function<void(CameraStatisticImp::Ptr &stats)> &invoker) {
    File::scanDir(_record_path, [&](const string &path, bool isDir) {
        if (isDir) {
            auto saved_path = path + "/info.txt";
            if (File::fileExist(saved_path) && File::fileSize(saved_path) > 0) {
                DebugL << "Found saved file: " << saved_path << ". Loading...";
                try {
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
                } catch (const exception &ex) {
                    WarnL << "Load saved file failed: " << saved_path << ": " << ex.what();
                }
                return true;
            }
            WarnL << "Saved file empty or invalid format: " << saved_path << ". Ignore";
            File::delete_file(saved_path, true);
        }
        return true;
    });
}

void StatisticRecorder::loadSavedSpeakerStatistics(const std::function<void(SpeakerStatisticImp::Ptr &stats)> &invoker) {
    File::scanDir(_speaker_path, [&](const string &path, bool isDir) {
        if (isDir) {
            auto saved_path = path + "/info.txt";
            if (File::fileExist(saved_path) && File::fileSize(saved_path) > 0) {
                DebugL << "Found saved file: " << saved_path << ". Loading...";
                auto imp = std::make_shared<SpeakerStatisticImp>(saved_path);
                weak_ptr<StatisticRecorder> weak_self = shared_from_this();
                imp->setOnRemove([weak_self](const string &device_id) {
                    auto strong_self = weak_self.lock();
                    if (!strong_self) {
                        return;
                    }
                    strong_self->removeSpeakerRecorder(device_id);
                });
                auto stats = imp->getParams();
                {
                    std::lock_guard<std::mutex> lock(_mtx_stats);
                    _sp_stats_map.emplace(stats.tuple.device_id, imp);
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

void StatisticRecorder::addMotionKeepThreshold(const std::string &device_id, bool start,  uint64_t threshold) {
    auto recorder = getRecorder(device_id, false);
    if (!recorder) {
        WarnL << "Received motion keep threshold update for device " << device_id << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addMotionKeepThreshold(start, threshold);
}

void StatisticRecorder::addTierKeepThreshold(const std::string &device_id, int tier_type, bool start, uint64_t threshold) {
    auto recorder = getRecorder(device_id, false);
    if (!recorder) {
        WarnL << "Received tier keep threshold update for device " << device_id << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addTierKeepThreshold(tier_type, start, threshold);
}

static void* s_tag;

static onceToken g_token(
[]() {
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastMotionKeepThreshold, [](BroadcastMotionKeepThresholdArgs) {
        auto recorder = StatisticRecorder::Instance().getRecorder(device_id, false);
        if (!recorder) {
            WarnL << "Received motion keep threshold update for device " << device_id << " but no statistic recorder found. Ignore update.";
            return;
        }
        recorder->addMotionKeepThreshold(start, threshold);
    });
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastTierKeepThreshold, [](BroadcastTierKeepThresholdArgs) {
        auto recorder = StatisticRecorder::Instance().getRecorder(args.device_id, false);
        if (!recorder) {
            WarnL << "Received tier keep threshold update for device " << args.device_id << " but no statistic recorder found. Ignore update.";
            return;
        }
        recorder->addTierKeepThreshold(tier_type, start, threshold);
    });
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastStreamSettingChange, [](BroadcastStreamSettingChangeArgs) {
        auto recorder = StatisticRecorder::Instance().getRecorder(args.device_id, false);
        if (!recorder) {
            WarnL << "Received stream setting change for device " << args.device_id << " but no statistic recorder found. Ignore update.";
            return;
        }
        recorder->saveVideoEncoderConfig(profile_token, video_encoder_config);
    });
}, 
[]() {
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastMotionKeepThreshold);
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastTierKeepThreshold);
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastStreamSettingChange);
});

} // namespace managerkit