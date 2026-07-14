#ifndef SPEAKER_SPEAKERCONTROLLER_H
#define SPEAKER_SPEAKERCONTROLLER_H

#include "ext-plugin/IDevice.h"
#include "GenericIPSpeaker.h"
#include "AudioFileManager.h"

#include <string>
#include <vector>

namespace managerkit {

class SpeakerController : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<SpeakerController> {
public:
    using Ptr = std::shared_ptr<SpeakerController>;

    SpeakerController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller);

    ~SpeakerController() = default;

    void createTimer();

    void setListener(const std::shared_ptr<DeviceSourceEvent> &listener);

    bool isReady() const;

    void setupController(const SpeakerOption &option);

    void stopController();

    void validateCredential(const std::string& username, const std::string& password, const int port, OnDeviceResult cb);

    void playAudio(const std::string& fileId, const std::string& fileRemoteId, OnDeviceResult cb, bool autoDisconnect = true);

    void uploadAudio(const AudioFile& file, OnDeviceResult cb, bool autoDisconnect = true);

    void stopAudio(const std::string& remoteId, OnDeviceResult cb, bool autoDisconnect = true);

    void deleteAudio(const std::string& remoteId, OnDeviceResult cb, bool autoDisconnect = true);

    void listAudio(OnDeviceResult cb, bool autoDisconnect = true);

private:
    void onManager();

    void onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps);

    void doUploadAudio(const AudioFile& file, OnDeviceResult cb);

    void doDisconnect(bool result, const std::string& resultData, OnDeviceResult cb);

    void setupDeviceController(const SpeakerOption &option);

    std::vector<std::string> getVendorFeatures(const std::string& manufacturer);
private:
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    std::atomic<bool> _ready { false };
    std::string _err_msg;
    uint64_t _last_reconnect_time = 0;
    toolkit::Timer::Ptr _timer_ctr;
    OnvifControl::Ptr _onvif_ctr;
    IDevice::Ptr _device_ctr;
    IAudioPlayback* _audio_playback_ctr = nullptr;
    std::string _address;
    std::string _device_name;
    bool _requires_device_credential = false;
    ControllerOption _ctrl_option;
};

} // namespace managerkit

#endif // SPEAKER_SPEAKERCONTROLLER_H