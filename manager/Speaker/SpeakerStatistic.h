#ifndef SPEAKER_SPEAKERSTATISTIC_H
#define SPEAKER_SPEAKERSTATISTIC_H

#include <mutex>
#include "Local/FileRecorder.h"
#include "Common/SerialTaskQueue.h"
#include "GenericIPSpeaker.h"
#include "SpeakerController.h"

namespace managerkit {

struct SpeakerStatistic;
class SpeakerStatisticHelper {
public:
    static bool getParams(const std::string &json_str, SpeakerStatistic &stats);

    static std::string getParamsString(const SpeakerStatistic &stats);
};

struct SpeakerAudioRef {
    std::string id;
    std::string remoteId;
    uint64_t uploaded_at;
};

struct SpeakerStatistic {
    DeviceTuple tuple;
    SpeakerOption option;
    bool connect = false;
    std::string status;
    uint64_t created_at;
    uint64_t updated_at;
    DeviceCapabilities device_caps;
    std::map<std::string, SpeakerAudioRef> fileUploaded;

    SpeakerStatistic() {
        created_at = 0;
        updated_at = 0;
    }
};

class SpeakerStatisticImp : private SpeakerStatistic, public std::enable_shared_from_this<SpeakerStatisticImp> {
public:
    using Ptr = std::shared_ptr<SpeakerStatisticImp>;

    SpeakerStatisticImp(const std::string &src_path);

    ~SpeakerStatisticImp();

    void setOnRemove(const std::function<void(const std::string&)> &cb) { _on_remove = std::move(cb); }

    void setDeviceTuple(const DeviceTuple &input_tuple);

    void setSpeakerOption(const SpeakerOption &option);

    void addDeviceCapabilities(bool input_connect, std::string input_status, const DeviceCapabilities *input_caps = nullptr);

    std::string getAudioRemoteId(const std::string &fileId);

    void setAudioRemoteId(const std::string &fileId, const std::string &remoteId);

    AudioVolumeInfo getAudioVolumeInfo();

    void setAudioVolume(const int deviceVolume, const int configuredVolume);

public:
    SpeakerStatistic getParams();

    void remove();

    // Enables/disables writes of the speaker config to the cluster ESC database (speaker.sync_config_to_db
    // must also be on). Enabling publishes the current config (it may be unchanged since startup);
    // enabling again (every server config reload) refreshes it at most every 30 s.
    void setSyncMode(bool enable);

private:
    void setup(const std::string &src_path);

    void load();

    void save();

    void saveOnvifCredentials();

    // Called by config setters (tuple / option): pushes the ESC resource now, bypassing the throttle.
    void markEscConfigChanged();

    // Queue an ESC upsert of the current config if it is pending. force=false applies the
    // retry throttle. Caller must hold _mtx.
    bool syncToEsc(bool force);

    // Queue ESC removal after all pending ESC tasks of this speaker. Caller must hold _mtx.
    void removeFromEsc();

private:
    std::mutex _mtx;
    FileRecorder<SpeakerStatistic, SpeakerStatisticHelper>::Ptr _file;
    std::function<void(const std::string&)> _on_remove;
    int _sync_interval_sec = 30;
    bool _sync_mode = false;
    uint64_t _last_sync_time = 0;
    bool _esc_dirty = false;    // config not yet written to ESC (never synced or last write failed)
    bool _esc_removed = false;  // remove() was called; no more ESC writes
    SerialTaskQueue::Ptr _esc_queue;
};

} // namespace managerkit

#endif // SPEAKER_SPEAKERSTATISTIC_H