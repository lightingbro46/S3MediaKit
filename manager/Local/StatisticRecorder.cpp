#include "StatisticRecorder.h"
#include <map>
#include "Common/config.h"
#include "Util/TimeTicker.h"

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
    _stats_map.clear();
}

void StatisticRecorder::registerType(const DeviceStatisticType &type) {
    CHECK(!type.schema.empty() && type.create);
    std::lock_guard<std::mutex> lock(_mtx_stats);
    _types[type.schema] = type;
}

void StatisticRecorder::setOnRemove(const DeviceStatisticBase::Ptr &stats) {
    weak_ptr<StatisticRecorder> weak_self = shared_from_this();
    weak_ptr<DeviceStatisticBase> weak_stats = stats;
    stats->setOnRemove([weak_self, weak_stats](const string &device_id) {
        auto strong_self = weak_self.lock();
        auto strong_stats = weak_stats.lock();
        if (!strong_self || !strong_stats) {
            return;
        }
        std::lock_guard<std::mutex> lock(strong_self->_mtx_stats);
        auto it = strong_self->_stats_map.find(device_id);
        if (it != strong_self->_stats_map.end() && it->second == strong_stats) {
            strong_self->_stats_map.erase(it);
        }
    });
}

bool StatisticRecorder::savedSchemaMatches(const string &device_dir, const string &schema) {
    auto saved_path = device_dir + "/info.txt";
    if (!File::fileExist(saved_path) || File::fileSize(saved_path) == 0) {
        return true;
    }
    DeviceStatisticHeader header;
    if (!parseDeviceStatisticHeader(File::loadFile(saved_path), header)) {
        // Unreadable: same as before schemas existed, the new statistic overwrites it.
        return true;
    }
    if (header.schema != schema) {
        ErrorL << "Device id " << header.id << " is already used by " << saved_path << " (schema " << header.schema
               << "), cannot create " << schema;
        return false;
    }
    return true;
}

void StatisticRecorder::loadAll() {
    Ticker ticker;
    std::unordered_map<std::string, DeviceStatisticType> types;
    {
        std::lock_guard<std::mutex> lock(_mtx_stats);
        types = _types;
    }
    std::map<std::string, size_t> loaded_count;
    File::scanDir(_record_path, [&](const string &path, bool isDir) {
        if (!isDir) {
            return true;
        }
        auto saved_path = path + "/info.txt";
        if (!File::fileExist(saved_path) || File::fileSize(saved_path) == 0) {
            WarnL << "Saved file empty or invalid format: " << saved_path << ". Ignore";
            File::delete_file(saved_path, true);
            return true;
        }
        try {
            DeviceStatisticHeader header;
            if (!parseDeviceStatisticHeader(File::loadFile(saved_path), header)) {
                WarnL << "Saved file is not valid JSON: " << saved_path << ". Ignore";
                return true;
            }
            auto type = types.find(header.schema);
            if (type == types.end()) {
                WarnL << "Saved file has unknown schema " << header.schema << ": " << saved_path << ". Ignore";
                return true;
            }
            if (header.schema_version > kDeviceStatisticSchemaVersion) {
                WarnL << "Saved file has schema version " << header.schema_version << " newer than "
                      << kDeviceStatisticSchemaVersion << ": " << saved_path << ". Ignore";
                return true;
            }
            if (header.id.empty()) {
                WarnL << "Saved file has no device id: " << saved_path << ". Ignore";
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(_mtx_stats);
                if (_stats_map.find(header.id) != _stats_map.end()) {
                    ErrorL << "Device id " << header.id << " of " << saved_path << " is already loaded. Ignore";
                    return true;
                }
            }
            DebugL << "Found saved file: " << saved_path << ". Loading...";
            auto stats = type->second.create(path);
            if (!stats) {
                WarnL << "Load saved file failed: " << saved_path;
                return true;
            }
            setOnRemove(stats);
            {
                std::lock_guard<std::mutex> lock(_mtx_stats);
                if (!_stats_map.emplace(header.id, stats).second) {
                    ErrorL << "Device id " << header.id << " of " << saved_path << " is already loaded. Ignore";
                    return true;
                }
            }
            ++loaded_count[header.schema];
            if (auto loader = type->second.loader.lock()) {
                loader->addSaved(stats);
            }
        } catch (const exception &ex) {
            WarnL << "Load saved file failed: " << saved_path << ": " << ex.what();
        }
        return true;
    });

    string counts;
    for (const auto &it : loaded_count) {
        counts += " " + it.first + "=" + to_string(it.second);
    }
    InfoL << "Loaded all saved devices:" << (counts.empty() ? " none" : counts) << ". " << ticker.elapsedTime() << " ms elapsed";
    for (const auto &it : types) {
        if (auto loader = it.second.loader.lock()) {
            loader->onSavedLoaded();
        }
    }
}

CameraStatisticImp::Ptr StatisticRecorder::getRecorder(const string &device_id, bool create_if_not_exist) {
    CHECK(!device_id.empty());
    return create_if_not_exist ? create<CameraStatisticImp>(GENERIC_RTSP_CAMERA_SCHEMA, device_id) : get<CameraStatisticImp>(device_id);
}

bool StatisticRecorder::removeRecorder(const string &device_id) {
    return remove<CameraStatisticImp>(device_id);
}

SpeakerStatisticImp::Ptr StatisticRecorder::getSpeakerRecorder(const string &device_id, bool create_if_not_exist) {
    CHECK(!device_id.empty());
    return create_if_not_exist ? create<SpeakerStatisticImp>(GENERIC_IP_SPEAKER_SCHEMA, device_id) : get<SpeakerStatisticImp>(device_id);
}

bool StatisticRecorder::removeSpeakerRecorder(const string &device_id) {
    return remove<SpeakerStatisticImp>(device_id);
}

PushCameraStatisticImp::Ptr StatisticRecorder::getPushCameraRecorder(const string &device_id, bool create_if_not_exist) {
    CHECK(!device_id.empty());
    return create_if_not_exist ? create<PushCameraStatisticImp>(GENERIC_PUSH_CAMERA_SCHEMA, device_id) : get<PushCameraStatisticImp>(device_id);
}

void StatisticRecorder::addArchiveSize(const string &device_id, const string &stream_id, size_t count, size_t size, uint64_t archive_start_time, uint64_t archive_end_time, bool add) {
    auto recorder = getRecording(device_id);
    if (!recorder) {
        WarnL << "Received archive size update for device " << device_id << " stream "<< stream_id << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addArchiveSize(stream_id, count, size, archive_start_time, archive_end_time, add);
}

void StatisticRecorder::addBookmarkCount(const string &device_id, uint64_t created_at, bool add) {
    auto recorder = getRecording(device_id);
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
    auto recorder = getRecording(device_id);
    if (!recorder) {
        WarnL << "Received stream statistic update for device " << device_id << " stream type " << stream_type << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addStreamStatistic(stream_type, live, status, info);
}

void StatisticRecorder::addMotionKeepThreshold(const std::string &device_id, bool start,  uint64_t threshold) {
    auto recorder = getRecording(device_id);
    if (!recorder) {
        WarnL << "Received motion keep threshold update for device " << device_id << " but no statistic recorder found. Ignore update.";
        return;
    }
    recorder->addMotionKeepThreshold(start, threshold);
}

void StatisticRecorder::addTierKeepThreshold(const std::string &device_id, int tier_type, bool start, uint64_t threshold) {
    auto recorder = getRecording(device_id);
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
        auto recorder = StatisticRecorder::Instance().getRecording(device_id);
        if (!recorder) {
            WarnL << "Received motion keep threshold update for device " << device_id << " but no statistic recorder found. Ignore update.";
            return;
        }
        recorder->addMotionKeepThreshold(start, threshold);
    });
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastTierKeepThreshold, [](BroadcastTierKeepThresholdArgs) {
        auto recorder = StatisticRecorder::Instance().getRecording(args.device_id);
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