#ifndef CAMERA_CAMERASTATISTIC_H
#define CAMERA_CAMERASTATISTIC_H

#include <mutex>
#include "Local/FileRecorder.h"
#include "GenericRtspCamera.h"
#include "CameraController.h"

namespace managerkit {

struct BookmarkStats {
    size_t recordAverageSizeB = 0;
    size_t recordCount = 0;
};

struct StreamStorageStats {
    size_t archiveIndexRecordCount = 0;
    size_t archiveSizeB = 0;
    uint64_t archiveStartTime = 0;
    uint64_t archiveEndTime = 0;
};

struct StreamStatistic {
    bool live = false;
    uint64_t last_change_status = 0;
    std::string status;
    int byte_speed = 0;
    bool has_video = false;
    std::string vcodec;
    int width = 0;
    int height = 0;
    int bitrate = 0;
    float fps = 0.0;
    bool has_audio = false;
    std::string acodec;
    int sample_rate = 0;
    int channel_no = 0;
    int sample_bit = 0;
    // bool transcoding_required = false;
    // int encoder_index = -1;
    // std::vector<std::string> transports;
};

struct DeviceStatistic {
    bool connect = false;
    std::string status;
    DeviceCapabilities device_caps;
    OnvifPTZProfile::PTZPresetMap user_presets;
    VideoEncoderConfig::VideoEncoderConfigMap stream_settings;
};

struct MotionStorageStats {
    uint64_t archiveStartTime = 0;
    uint64_t archiveEndTime = 0;
};

struct TierStorageStats {
    uint64_t archiveStartTime = 0;
    uint64_t archiveEndTime = 0;
};

struct CameraStatistic;
class CameraStatisticHelper {
public:
    static bool getParams(const std::string &json_str, CameraStatistic &stats);

    static std::string getParamsString(const CameraStatistic &stats);
};

struct CameraStatistic {
    DeviceTuple tuple;
    CameraOption option;
    std::unordered_map<int, StreamTuple> stream_map;
    BookmarkStats bm;
    std::unordered_map<std::string, StreamStorageStats> storage_map;
    std::unordered_map<int, StreamStatistic> sinfo_map;
    MotionStorageStats motion_stats;
    std::unordered_map<int, TierStorageStats> tier_storage_map;
    DeviceStatistic device_stats;
    uint64_t created_at;
    uint64_t updated_at;

    CameraStatistic() {
        created_at = 0;
        updated_at = 0;
    }
};

class CameraStatisticImp : private CameraStatistic {
public:
    using Ptr = std::shared_ptr<CameraStatisticImp>;

    CameraStatisticImp(const std::string &src_path, int sync_interval_sec = 30);
    
    ~CameraStatisticImp();

    void setOnRemove(const std::function<void(const std::string&)> &cb) { _on_remove = std::move(cb); }

    void setDeviceTuple(const DeviceTuple &input_tuple);

    void setStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map);

    void setCameraOption(const CameraOption &input_option);

    std::string getStreamID(const int &stream_type);

    std::unordered_map<std::string, VideoEncoderConfig> getStreamVideoEncoderConfigMap();

    void addArchiveSize(std::string stream_id, size_t count, size_t size, uint64_t archived_start_time, uint64_t archived_end_time, bool add = true);

    void addBookmarkCount(uint64_t bm_created_at,  size_t size, bool add = true);

    void addStreamStatistic(int stream_type, bool live, std::string status, const mediakit::TranslationInfo *info = nullptr);

    void addDeviceCapabilities(bool connect, std::string status, const DeviceCapabilities *device_caps = nullptr);

    void addUserPresets(const std::string &preset_token, const std::string &preset_name, float abs_pan, float abs_tilt, float abs_zoom, bool add = true);

    void addMotionKeepThreshold(bool start, uint64_t threshold);

    void addTierKeepThreshold(int tier_type, bool start, uint64_t threshold);

    void saveVideoEncoderConfig(const std::string token, const VideoEncoderConfig &config);

    bool loadVideoEncoderConfig(const std::string token, VideoEncoderConfig &config);

public:
    CameraStatistic getParams();

    void remove(bool failover_active = false);

    void setSyncMode(bool enable);

    static bool syncFromEsc(std::string &guid, CameraStatistic &resource);

private:
    void setup(const std::string &src_path);

    void load();

    void save();

    bool syncToEsc();

    bool removeFromEsc();

    void assignResource(bool regist = true);

    void syncResourceStatus();

private:
    std::mutex _mtx;
    FileRecorder<CameraStatistic, CameraStatisticHelper>::Ptr _file;
    std::function<void(const std::string&)> _on_remove;
    int _sync_interval_sec = 30;
    bool _sync_mode = false;
    uint64_t _last_sync_time = 0;
};

} // namespace managerkit

#endif // CAMERA_CAMERASTATISTIC_H