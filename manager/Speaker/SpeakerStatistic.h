#ifndef SPEAKER_SPEAKERSTATISTIC_H
#define SPEAKER_SPEAKERSTATISTIC_H

#include <mutex>
#include "Local/FileRecorder.h"
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

class SpeakerStatisticImp : private SpeakerStatistic {
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

    bool isFeatureSupported(mediakit::SupportedFeatures feature);

public:
    SpeakerStatistic getParams();

    void remove();

private:
    void setup(const std::string &src_path);

    void load();

    void save();

private:
    std::mutex _mtx;
    FileRecorder<SpeakerStatistic, SpeakerStatisticHelper>::Ptr _file;
    std::function<void(const std::string&)> _on_remove;
};

} // namespace managerkit

#endif // SPEAKER_SPEAKERSTATISTIC_H