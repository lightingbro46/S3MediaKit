#ifndef SPEAKER_SPEAKERCONTROLLER_H
#define SPEAKER_SPEAKERCONTROLLER_H

#include "ext-plugin/IDevice.h"
#include "Control/DeviceControl.h"
#include "Control/VendorStage.h"
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

    // The control protocol is connected (backchannel, device information).
    bool isReady() const;

    // Owner poller. Error for a command while the speaker is not ready: why the last connect or probe
    // failed (credentials, network, clock) when known, else CODE_DEVICE_OFFLINE.
    toolkit::SockException notReadyError() const;

    // Owner poller. The speaker has a vendor plugin for audio files and volume (vendor API channel).
    bool hasVendorApi() const { return _audio_playback_ctr != nullptr; }
    // The vendor API answered its last check or operation: files and volume work, whatever isReady() says.
    bool isVendorReady() const { return _vendor_ready.load(); }
    // Owner poller. Error for a vendor operation while !isVendorReady(), as notReadyError().
    toolkit::SockException vendorNotReadyError() const;

    // Capabilities saved before a restart: reported, merged with the vendor channel, until a control
    // protocol connects. Call before the first setupController().
    void restoreCapabilities(const DeviceCapabilities &caps);

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

    // VendorStage::probe() for speakers with the configured account; `cb` runs once, on any thread.
    // Unit tests override it.
    virtual void probeVendorApis(const std::string &ip, int port, const std::string &username,
                                 const std::string &password, const VendorStage::OnProbe &cb);

    // The vendor plugin of `cfg` (null without one). Unit tests override it.
    virtual IDevice::Ptr createVendorDevice(const SpeakerConfig &cfg);

private:
    friend class SpeakerControllerTestPeer;

    void onManager();

    // The connect part of onManager(): connect the controller, or probe the protocols without one.
    void runControlCycle();

    // ── Manufacturer detection (on _poller), for a speaker configured without one ──
    // One cycle: the vendor APIs, then the control protocols (runControlCycle()).
    void startDetect();
    void onVendorProbeDone(uint64_t generation, const VendorStage::ProbeResult &result);
    // A control protocol connected while detecting: its device information names the manufacturer.
    void onProtocolIdentified(const DeviceControl::Ptr &device_ctr, const std::string &manufacturer);
    // _vendor_stage detected the brand: drive the speaker through its plugin (if any). The name reaches
    // the backend in the capabilities (vendor channel, or deviceInfo once a control protocol connects).
    void onBrandDetected();
    // A detection cycle ended without a controller; `unreachable`: no control protocol answered.
    void onDetectCycleFailed(bool unreachable);
    // Seconds between cycles: VendorStage::cycleIntervalSec() with speaker.detect_max_interval_sec.
    int cycleIntervalSec() const;

    // Publishes the state of the control protocol (`connect`, `status`) with the capabilities of both
    // channels: `caps` from a control protocol replaces its part, null keeps the last one.
    void onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps);

    // ── Vendor API channel (on _poller), beside the control protocol ──
    // The last protocol capabilities with the vendor channel's state, device information and volume.
    DeviceCapabilities currentCaps() const;
    // Forgets the vendor channel's state; checks or operations started before are dropped.
    void resetVendorChannel();
    // Logs in and out with a fresh plugin instance every 60 s (rejected: speaker.control_auth_retry_sec),
    // a session of its own so it never ends one of a running operation.
    void checkVendorChannel(uint64_t now);
    // `ok`: the vendor API answered (a check, or the login of an operation); `device` read when ok.
    void onVendorResult(uint64_t generation, bool ok, const std::string &msg, DeviceFailure failure,
                        const IDevice::Ptr &device);
    SpeakerConfig vendorConfig() const;

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
    // `manufacturer`: the device information of a successful connect.
    void onConnectResult(const DeviceControl::Ptr &device_ctr, ConnectError err, const std::string &manufacturer = "");
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
    // Brand and vendor plugin the speaker is driven with: from the manufacturer option, or detected
    // while it is blank (each cycle then runs startDetect()). _poller only.
    VendorStage _vendor_stage;
    // Vendor API channel (_poller only, except _vendor_ready).
    std::atomic<bool> _vendor_ready { false };
    ReconnectPacer _vendor_pacer;
    bool _vendor_checking = false;
    uint64_t _vendor_generation = 0;   // bumped by resetVendorChannel()
    std::string _vendor_status;        // why the vendor API did not answer, "" when it did
    DeviceFailure _vendor_down = DeviceFailure::None; // lastFailure() of that answer (vendorNotReadyError())
    bool _vendor_seen = false;         // answered once: plays files (kept while it is down)
    bool _vendor_known = false;        // _vendor_info and _vendor_audio were read
    IDeviceInfo _vendor_info;
    AudioSettings _vendor_audio;
    // Capabilities of the control protocol: the last connect, else the saved ones (restoreCapabilities()).
    DeviceCapabilities _protocol_caps;
    uint64_t _last_clock_sync = 0; // when the maintenance cycle last checked the clock; _poller only
};

} // namespace managerkit

#endif // SPEAKER_SPEAKERCONTROLLER_H