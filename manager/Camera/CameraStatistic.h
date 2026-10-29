#ifndef CAMERA_CAMERASTATISTIC_H
#define CAMERA_CAMERASTATISTIC_H

#include <mutex>
#include <vector>
#include <unordered_set>
#include "Local/FileRecorder.h"
#include "Local/DeviceStatistic.h"
#include "RecordingStatistic.h"
#include "Poller/EventPoller.h"
#include "Common/SerialTaskQueue.h"
#include "GenericRtspCamera.h"
#include "CameraController.h"

namespace managerkit {

struct DeviceStatistic {
    bool connect = false;
    std::string status;
    DeviceCapabilities device_caps;
    OnvifPTZProfile::PTZPresetMap user_presets;
    VideoEncoderConfig::VideoEncoderConfigMap stream_settings;
};

struct CameraStatistic;
class CameraStatisticHelper {
public:
    static bool getParams(const std::string &json_str, CameraStatistic &stats);

    static std::string getParamsString(const CameraStatistic &stats);
};

struct CameraStatistic : public RecordingStatistic {
    CameraOption option;
    DeviceStatistic device_stats;

    CameraStatistic() { schema = GENERIC_RTSP_CAMERA_SCHEMA; }
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

class CameraStatisticImp : private CameraStatistic, public RecordingStatisticImp, public std::enable_shared_from_this<CameraStatisticImp> {
public:
    using Ptr = std::shared_ptr<CameraStatisticImp>;

    CameraStatisticImp(const std::string &src_path, int sync_interval_sec = 30);
    
    ~CameraStatisticImp();

    std::string schema() override { return CameraStatistic::schema; }

    std::string deviceId() override;

    void setOnRemove(const std::function<void(const std::string&)> &cb) override { _on_remove = cb; }

    void setDeviceTuple(const DeviceTuple &input_tuple);

    void setStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map);

    void setCameraOption(const CameraOption &input_option);

    void setTransportStats(const TransportStats &input_stats);

    std::unordered_map<std::string, VideoEncoderConfig> getStreamVideoEncoderConfigMap();

    // Also writes the stream status history and the ESC status.
    void addStreamStatistic(int stream_type, bool live, std::string status, const mediakit::TranslationInfo *info = nullptr) override;

    void addDeviceCapabilities(bool connect, std::string status, const DeviceCapabilities *device_caps = nullptr);

    void addUserPresets(const std::string &preset_token, const std::string &preset_name, float abs_pan, float abs_tilt, float abs_zoom, bool add = true);

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
    RecordingStatistic &recordingData() override { return *this; }

    RecordingOption &recordingOption() override { return option; }

    void saveRecording() override { save(); }

    void setup(const std::string &src_path);

    void load();

    enum class SaveMode {
        Async,   // statistics: info.txt is written later on the ESC queue, coalesced per camera
        Sync,    // config changes: info.txt is written before returning
        EscOnly, // nothing to persist (only a measurement changed); only the ESC part below runs
    };

    // Persist info.txt. Also retries a pending ESC sync (throttled) if the last one failed.
    // Caller must hold _mtx.
    void save(SaveMode mode = SaveMode::Async);

    // Writes `snapshot` unless the file was removed or a newer snapshot is already on disk.
    void writeFile(const CameraStatistic &snapshot, uint64_t seq);

    // Runs on the ESC queue: writes the latest state if a save is still queued.
    void flushQueuedSave();

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
    FileRecorder<CameraStatistic, CameraStatisticHelper>::Ptr _file;
    uint64_t _file_seq = 0;            // bumped by every save() (guarded by _mtx)
    bool _file_save_queued = false;    // an async write is waiting on the ESC queue (guarded by _mtx)
    std::mutex _file_mtx;              // serializes info.txt writes and remove(); taken after _mtx
    uint64_t _file_written_seq = 0;    // guarded by _file_mtx
    bool _file_removed = false;        // guarded by _file_mtx
    std::function<void(const std::string&)> _on_remove;
    int _sync_interval_sec = 30;
    bool _sync_mode = false;
    uint64_t _last_sync_time = 0;
    bool _esc_dirty = false;    // config not yet written to ESC (never synced or last write failed)
    bool _esc_removed = false;  // remove() was called; no more ESC writes
    SerialTaskQueue::Ptr _esc_queue;
    std::unordered_set<int> _status_history_initialized_streams;
    bool _device_status_history_initialized = false;
};

} // namespace managerkit

#endif // CAMERA_CAMERASTATISTIC_H
