#ifndef CAMERA_CAMERASTATISTIC_H
#define CAMERA_CAMERASTATISTIC_H

#include <mutex>
#include <vector>
#include <unordered_set>
#include "Local/FileRecorder.h"
#include "Poller/EventPoller.h"
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

struct TransportStats {
    std::vector<std::string> liveTransports;
    std::vector<std::string> replayTransports;
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
    TransportStats transport_stats;
    std::unordered_map<int, TierStorageStats> tier_storage_map;
    DeviceStatistic device_stats;
    uint64_t created_at;
    uint64_t updated_at;

    CameraStatistic() {
        created_at = 0;
        updated_at = 0;
    }
};

struct ReduceQualityCapability {
    bool supported = false;
    std::vector<mediakit::Broadcast::ReduceQualityProfile> profiles;

    bool operator==(const ReduceQualityCapability &other) const {
        return supported == other.supported && profiles == other.profiles;
    }
};

class ReduceQualityCapabilityBuilder {
public:
    static ReduceQualityCapability build(const CameraOption &option,
                                         const std::unordered_map<int, StreamTuple> &stream_map,
                                         const std::unordered_map<int, StreamStatistic> &statistics);
    static Json::Value toJson(const ReduceQualityCapability &capability);
};

class CameraStatisticImp : private CameraStatistic, public std::enable_shared_from_this<CameraStatisticImp> {
public:
    using Ptr = std::shared_ptr<CameraStatisticImp>;

    CameraStatisticImp(const std::string &src_path, int sync_interval_sec = 30);
    
    ~CameraStatisticImp();

    void setOnRemove(const std::function<void(const std::string&)> &cb) { _on_remove = std::move(cb); }

    void setDeviceTuple(const DeviceTuple &input_tuple);

    void setStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map);

    void setCameraOption(const CameraOption &input_option);

    void setTransportStats(const TransportStats &input_stats);

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

    // ONVIF profile token whose stream URL matches the primary stream, "" when it cannot be resolved.
    std::string getPrimaryProfileToken() const;

public:
    CameraStatistic getParams();

    void remove(bool failover_active = false);

    // Enables/disables writes to the cluster ESC database. Enabling publishes the current
    // ownership, config and status (the option may be unchanged since startup).
    void setSyncMode(bool enable);

    // The camera is leaving this node (moved, failed back or deleted): release this node's
    // assignment and stop ESC writes, without publishing the local config.
    void detachFromCluster();

    static bool syncFromEsc(std::string &guid, CameraStatistic &resource);

private:
    void setup(const std::string &src_path);

    void load();

    // Persist info.txt. Also retries a pending ESC sync (throttled) if the last one failed.
    void save();

    // Called by config setters (tuple / stream urls / option): pushes the ESC resource now,
    // bypassing the throttle, so config changes are never delayed or dropped.
    void markEscConfigChanged();

    // Queue an ESC upsert of the current config if it is pending. force=false applies the
    // retry throttle. Caller must hold _mtx.
    bool syncToEsc(bool force);

    // Queue ESC removal after all pending ESC tasks of this camera.
    void removeFromEsc();

    // Run blocking ESC/SQLite work off the camera poller, in FIFO order per camera.
    void postEscTask(const std::function<void()> &task);

    void assignResource(bool regist = true);

    void syncResourceStatus();

    void saveDeviceStatusHistory(const std::string &device_id, const std::string &device_url,
                                 bool connect, const std::string &reason);

    void saveStreamStatusHistory(const std::string &stream_id, const std::string &stream_url,
                                 bool connect, const std::string &reason);

private:
    std::mutex _mtx;
    FileRecorder<CameraStatistic, CameraStatisticHelper>::Ptr _file;
    std::function<void(const std::string&)> _on_remove;
    int _sync_interval_sec = 30;
    bool _sync_mode = false;
    uint64_t _last_sync_time = 0;
    bool _esc_dirty = false;    // config not yet written to ESC (never synced or last write failed)
    bool _esc_removed = false;  // remove() was called; no more ESC writes
    toolkit::EventPoller::Ptr _esc_poller;
    std::unordered_set<int> _status_history_initialized_streams;
    bool _device_status_history_initialized = false;
};

} // namespace managerkit

#endif // CAMERA_CAMERASTATISTIC_H
