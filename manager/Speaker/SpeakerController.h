#ifndef SPEAKER_SPEAKERCONTROLLER_H
#define SPEAKER_SPEAKERCONTROLLER_H

#include "ext-plugin/IDevice.h"
#include "GenericIPSpeaker.h"
#include "AudioFileManager.h"
#include "Common/SerialTaskQueue.h"

#include <string>
#include <vector>

namespace managerkit {

class SpeakerController : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<SpeakerController> {
public:
    using Ptr = std::shared_ptr<SpeakerController>;

    SpeakerController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller);

    ~SpeakerController() = default;

    static void probe(const std::string& ip, int port,
                      const std::string& username, const std::string& password,
                      std::function<void(const toolkit::SockException&, managerkit::IDevice::Ptr)> cb,
                      size_t brandIndex = 0);

    void createTimer();

    void setListener(const std::shared_ptr<DeviceSourceEvent> &listener);

    bool isReady() const;

    void setupController(const SpeakerOption &option);

    void stopController();

    // ================== Vendor-specific operations ==================
    void playAudioFileByVendorApi(const std::string& fileId, const std::string& fileRemoteId, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void uploadAudio(const AudioFile& file, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void stopAudio(const std::string& remoteId, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void deleteAudio(const std::string& remoteId, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void listAudio(managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void setAudioVolume(int volume, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    // ================== Backchannel operations ==================
    void playAudioFileByBackchannel(const std::string& fileId, managerkit::OnDeviceResult cb);

private:
    void onManager();

    void onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps);

    void setupDeviceController();

    // ================== Vendor-specific operations ==================
    // Owner poller: the device a vendor operation runs on, or nullptr after answering `cb`.
    IDevice::Ptr vendorDevice(const OnDeviceResult &cb);

    static void doUploadAudio(const IDevice::Ptr &device, const managerkit::AudioFile& file, managerkit::OnDeviceResult cb);

    static void doDisconnect(const IDevice::Ptr &device, bool result, const std::string& resultData, OnDeviceResult cb);

    // ================== Backchannel operations ==================
    // Device queue: resolves the backchannel RTSP URI and registers it with BackchannelManager.
    // Takes the controller and credentials explicitly because it runs off the owner poller.
    void registerAudioBackchannel(const OnvifControl::Ptr &onvif_ctr, const std::string &username,
                                  const std::string &password, bool sharedDspPipeline);
private:
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    // Serializes blocking calls to this speaker (ONVIF SOAP) on the shared `device` pool.
    SerialTaskQueue::Ptr _device_queue;
    std::atomic<bool> _ready { false };
    std::string _err_msg;
    uint64_t _last_reconnect_time = 0;
    toolkit::Timer::Ptr _timer_ctr;
    OnvifControl::Ptr _onvif_ctr;
    // ONVIF account _onvif_ctr logs in with (speaker.onvifUsername on vendors that need their own account).
    std::string _onvif_username;
    IDevice::Ptr _device_ctr;
    IAudioPlayback* _audio_playback_ctr = nullptr;
    std::string _address;
    ControllerOption _ctrl_option;
    std::string _device_name;
};

} // namespace managerkit

#endif // SPEAKER_SPEAKERCONTROLLER_H