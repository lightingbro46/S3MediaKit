#ifndef SPEAKER_SPEAKERCONTROLLER_H
#define SPEAKER_SPEAKERCONTROLLER_H

#include "ext-plugin/IDevice.h"
#include "Control/DeviceControl.h"
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

    virtual ~SpeakerController() = default;

    static void probe(const std::string& ip, int port,
                      const std::string& username, const std::string& password,
                      std::function<void(const toolkit::SockException&, managerkit::IDevice::Ptr)> cb,
                      size_t brandIndex = 0);

    void createTimer();

    void setListener(const std::shared_ptr<DeviceSourceEvent> &listener);

    bool isReady() const;

    // Owner poller. Error for a command while the speaker is not ready: why the last connect or probe
    // failed (credentials, network, clock) when known, else CODE_DEVICE_OFFLINE.
    toolkit::SockException notReadyError() const;

    // Reads the device clock last measured by a rejected request (for CODE_DEVICE_CLOCK_SKEW responses).
    // Get it on _poller; the returned function may run on any thread. Null without a controller.
    std::function<DeviceClockReading()> clockReader() const;

    void setupController(const SpeakerOption &option);

    // Protocol the speaker was controlled with before ("ONVIF", "PSIA", "ISAPI", "" = unknown), from the
    // saved capabilities: an AUTO speaker starts locked to it instead of probing. Call before setupController().
    void setLastControlProtocol(const std::string &protocol);

    // Owner poller. Runs the reconnect cycle now, also while the speaker is waiting out a rejected
    // password (speaker.control_auth_retry_sec), and reports the outcome of that attempt.
    void reconnectNow(const ReconnectCallback &cb);

    void stopController();

    // ================== Vendor-specific operations ==================
    void playAudioFileByVendorApi(const std::string& fileId, const std::string& fileRemoteId, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void uploadAudio(const AudioFile& file, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void stopAudio(const std::string& remoteId, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void deleteAudio(const std::string& remoteId, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void listAudio(managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    void setAudioVolume(int volume, managerkit::OnDeviceResult cb, bool autoDisconnect = true);

    // Why the last vendor operation failed (IDevice::lastFailure() when it ended), None after a
    // success or a failure that was not the network or the credentials. Any thread.
    DeviceFailure lastVendorFailure() const { return static_cast<DeviceFailure>(_vendor_failure.load()); }

    // ================== Backchannel operations ==================
    void playAudioFileByBackchannel(const std::string& fileId, managerkit::OnDeviceResult cb);

protected:
    // Creates the device control of `protocol` (null when not supported); unit tests override it.
    virtual DeviceControl::Ptr createDeviceControl(ControlProtocol protocol, const std::string &address,
                                                         const std::string &username, const std::string &password);

private:
    friend class SpeakerControllerTestPeer;

    void onManager();

    void onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps);

    // False when the speaker cannot be controlled (vendor plugin missing or broken).
    bool setupDeviceController();

    // What a device task needs from the owner poller, copied there.
    struct ConnectContext {
        IDevice::Ptr vendor;   // vendor plugin, may be null
        std::string username;  // account the controller logs in with
        std::string password;
        bool sharedDsp = false;
        uint64_t generation = 0;
    };
    ConnectContext connectContext() const;

    // ── Control protocol selection (on _poller unless noted), as CameraController ──
    void startProtocolProbe();
    void onProbeDone(const DeviceControl::Ptr &device_ctr, uint64_t generation, const std::string &err_msg,
                     ConnectError err, DeviceFailure failure,
                     const DeviceClockReading &clock);
    void onConnectResult(const DeviceControl::Ptr &device_ctr, ConnectError err);
    void onAuthResult(ConnectError err);
    void unlockProtocol(const std::string &reason);
    // Answers the reconnectNow() requests waiting for the attempt that just ended (_poller).
    void finishReconnect(const ReconnectResult &result);
    // Device queue: hands `result` to finishReconnect() on _poller.
    void postReconnectResult(const ReconnectResult &result);
    // Device queue: capabilities, backchannel and the connected event after a successful connect().
    // `sync_clock`: set the device clock first (onvif.clockSyncEnable and the speaker's enableTimeSync).
    void publishConnected(const DeviceControl::Ptr &device_ctr, const ConnectContext &ctx, bool sync_clock);
    // onvif.clockSyncEnable and SpeakerOption::enableTimeSync: whether the server may set the clock.
    bool timeSyncEnabled() const;

    // ================== Vendor-specific operations ==================
    // Owner poller: the device a vendor operation runs on, or nullptr after answering `cb`.
    IDevice::Ptr vendorDevice(const OnDeviceResult &cb);

    // A vendor operation, run once the vendor session is up. It answers through `done`, which closes
    // the session first when the caller asked for autoDisconnect.
    typedef std::function<void(const IDevice::Ptr &device, IAudioPlayback *playback, const OnDeviceResult &done)> VendorOp;
    // Owner poller: vendorDevice() → connect → `op`; a failed connect answers `cb` directly.
    void runVendorOp(const VendorOp &op, const OnDeviceResult &cb, bool autoDisconnect);

    static void doUploadAudio(const IDevice::Ptr &device, const managerkit::AudioFile& file, managerkit::OnDeviceResult cb);

    static void doDisconnect(const IDevice::Ptr &device, bool result, const std::string& resultData, OnDeviceResult cb);

    // ================== Backchannel operations ==================
    // Device queue: resolves the backchannel RTSP URI and registers it with BackchannelManager.
    // Takes the controller and credentials explicitly because it runs off the owner poller.
    void registerAudioBackchannel(const DeviceControl::Ptr &device_ctr, const ConnectContext &ctx);
private:
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    // Serializes blocking calls to this speaker (ONVIF SOAP) on the shared `device` pool.
    SerialTaskQueue::Ptr _device_queue;
    std::atomic<bool> _ready { false };
    std::string _err_msg;
    // When the reconnect cycle runs next; slowed down while the speaker rejects the credentials.
    ReconnectPacer _pacer;
    std::vector<ReconnectCallback> _reconnect_waiters;
    toolkit::Timer::Ptr _timer_ctr;
    // The one controller of this speaker, whatever its protocol; only set or reset on _poller.
    DeviceControl::Ptr _device_ctr;
    // Account _device_ctr logs in with (speaker.onvifUsername on vendors that need their own account).
    std::string _ctrl_username;
    // setupController() accepted the speaker: there is a controller, or an AUTO probe to run.
    bool _configured = false;
    // Fixed protocol of the option (ONVIF for vendors whose plugin needs it), or the one AUTO locked to.
    ProtocolLock _lock;
    bool _probing = false;
    // lastFailure() of the controller the last failed AUTO probe reported; read when there is no controller.
    DeviceFailure _probe_failure = DeviceFailure::None;
    // Bumped whenever the controller is replaced, so a probe or connect started for an older one is dropped.
    std::atomic<uint64_t> _ctrl_generation { 0 };
    // Vendor plugin (audio files, volume, ONVIF account), for brands that have one.
    IDevice::Ptr _vendor_ctr;
    IAudioPlayback* _audio_playback_ctr = nullptr;
    std::atomic<int> _vendor_failure { static_cast<int>(DeviceFailure::None) };
    std::string _address;
    ControllerOption _ctrl_option;
    std::string _device_name;
    bool _enableTimeSync = true; // SpeakerOption::enableTimeSync; read on _poller only
    uint64_t _last_clock_sync = 0; // when the maintenance cycle last checked the clock; _poller only
};

} // namespace managerkit

#endif // SPEAKER_SPEAKERCONTROLLER_H