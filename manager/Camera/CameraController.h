#ifndef CAMERA_CAMERACONTROLLER_H
#define CAMERA_CAMERACONTROLLER_H

#include "GenericRtspCamera.h"
#include "Control/OnvifDeviceControl.h"
#include "Common/SerialTaskQueue.h"

namespace managerkit {

class CameraController : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<CameraController>  {
public:
    using Ptr = std::shared_ptr<CameraController>;

    CameraController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller);

    virtual ~CameraController() = default;

    void setListener(const std::shared_ptr<DeviceSourceEvent> &listener);

    void createTimer();

    bool isReady() const;

    void setupController(const CameraOption &option);

    void stopController();
    
    void PTZMove(const std::string &strDirect, int speed, const std::function<void(const toolkit::SockException &ex)> &cb);

    void ImageMoveControl(const std::string &strDirect, int speed, const std::function<void(const toolkit::SockException &ex)> &cb);

    void RelayOutputControl(const std::string &strDirect, const std::string &relayToken, const std::function<void(const toolkit::SockException &ex)> &cb);

    bool addUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex, float pan, float tilt, float zoom)> &cb);

    bool addUserPTZPreset(const std::string &presetToken, const std::string &presetName, float &pan, float &tilt, float &zoom);

    bool removeUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex)> &cb);

    void PTZGotoPreset(const std::string &presetToken, bool isUserPreset, const std::function<void(const toolkit::SockException &ex)> &cb);

    void setMediaProfileAsync(const std::string &profileToken, VideoEncoderConfig &config, const std::function<void(const toolkit::SockException &ex)> &cb);

    void getMediaProfileAsync(const std::string &profileToken, const std::function<void(const toolkit::SockException &ex, VideoEncoderConfig &config)> &cb);

    void addProfileConfig(const std::string &profileToken, const VideoEncoderConfig &config, bool emitEvent = true);

    void getSDCardInfoAsync(const std::function<void(const toolkit::SockException &ex, SDCardInformation &info)> &cb);

    // Video config rule jobs: a time-boxed encoder override on one profile, rolled back to the
    // baseline (the _profileConfigMap entry) when the job expires. While a rule owns a token, the
    // profile sync retries the rule config instead of the baseline.
    void applyRuleVideoConfigAsync(const std::string &logId, const std::string &profileToken, const VideoEncoderConfig &config, const std::function<void(const toolkit::SockException &ex)> &cb);

    void rollbackRuleVideoConfigAsync(const std::string &profileToken, const VideoEncoderConfig &baseline, const std::function<void(const toolkit::SockException &ex)> &cb);

    bool getBaselineVideoConfig(const std::string &profileToken, VideoEncoderConfig &config) const;

    void clearRuleVideoConfig(const std::string &profileToken);

    bool isRuleResultReported(const std::string &profileToken) const;

protected:
    // Creates the device control for `address`; unit tests override it to return a fake device.
    virtual OnvifControl::Ptr createOnvifControl(const std::string &address, const std::string &username,
                                                 const std::string &password);

private:
    friend class CameraControllerTestPeer; // unit tests drive the media profile sync directly

    void onManager();

    void onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps);

    // Media profile sync after (re)connect, in three steps so _profileConfigMap is only touched on
    // _poller: read the camera profiles (device queue) → decide per profile (_poller) → send the
    // needed set commands (device queue) → store the results (_poller).
    struct ProfileSetRequest {
        std::string token;
        VideoEncoderConfig based_on;   // map entry the decision was made from
        VideoEncoderConfig on_success; // stored when the set command succeeds
        VideoEncoderConfig on_failure; // stored when it fails
        bool is_rule = false;          // decision came from _ruleConfigMap, not _profileConfigMap
    };
    // Device queue: reads the profiles and hands them to applyMediaProfileSync on _poller.
    void syncMediaProfile(const OnvifControl::Ptr &onvif_ctr);
    void applyMediaProfileSync(const OnvifControl::Ptr &onvif_ctr, const OnvifMediaProfileMap &profiles);
    // _poller: stores a set result unless the entry changed meanwhile (e.g. setMediaProfileAsync).
    // `err_msg` carries the SOAP error of a failed set, reported for rule requests.
    void storeProfileSetResult(const ProfileSetRequest &request, bool ok, const std::string &err_msg = "");

    // _poller: puts the baseline of `profileToken` back into retry, used when a rollback could not
    // reach the camera so the profile sync pushes the baseline again later.
    void markBaselineNeedResync(const std::string &profileToken);

    // _poller: broadcasts the outcome of a rule job once; later calls for the same token are ignored.
    void reportRuleConfigResult(const std::string &profileToken, bool success, const std::string &err_code, const std::string &err_msg);

    // _poller: true while a profile config is still waiting to be pushed to the camera (NEW or
    // PROCESSING). Such a camera needs its profiles re-read on every maintenance cycle to drive
    // the retry engine, whatever `OnvifCtrl::kProfileResyncIntervalSec` says.
    bool hasPendingProfileConfig() const;

    // Runs a blocking device call (ONVIF SOAP) on this camera's serial queue.
    void postDeviceTask(SerialTaskQueue::Task task);

    // Same, but ahead of the maintenance work already queued for this camera and for the shared
    // `device` pool. For commands a user is waiting for (PTZ, presets, imaging, relay outputs):
    // both queues are FIFO, so without this a command waits behind the background cycle of every
    // other camera.
    void postPriorityDeviceTask(SerialTaskQueue::Task task);

    // Posts `task` to the serial queue after `delay_ms`; callable from any thread. The delay runs on
    // _poller. `on_cancel` runs instead when the controller is gone or, if `ptz_generation` is not
    // 0, when a newer PTZ command was issued meanwhile.
    void postDeviceTaskAfter(int delay_ms, uint64_t ptz_generation,
                             const SerialTaskQueue::Task &task, const SerialTaskQueue::Task &on_cancel);

private:
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    // Serializes all blocking calls to this camera on the shared `device` pool.
    SerialTaskQueue::Ptr _device_queue;
    // Incremented (on _poller) by every PTZ command; a pending continuous-move stop only fires
    // while it is unchanged, so it cannot stop a newer PTZ command.
    uint64_t _ptz_generation = 0;
    std::atomic<bool> _ready { false };
    std::string _err_msg;
    uint64_t _last_reconnect_time = 0;
    // Last time the maintenance cycle re-read the media profiles of this camera, and last time it
    // checked the camera clock; only touched on _poller. See OnvifCtrl::kProfileResyncIntervalSec
    // and OnvifCtrl::kClockSyncIntervalSec.
    uint64_t _last_profile_resync = 0;
    uint64_t _last_clock_sync = 0;
    toolkit::Timer::Ptr _timer_ctr;
    OnvifControl::Ptr _onvif_ctr;
    std::string _address;
    ControllerOption _ctrl_option;
    int _ptzMode = CameraOption::kPTZModeAuto;
    // default ptz speed, range [0,1], used when caller does not specify speed in PTZMove
    float _ptzSpeed = 0.5;
    bool _reversePanAxis = false;
    bool _reverseTiltAxis = false;
    bool _enablePTZControl = true;
    OnvifPTZProfile::PTZPresetMap _userPresets; // user defined preset list, used for preset operation when device does not support get preset api or preset info is incomplete
    // whether to keep config of profile and stream changed from camera web page, default false, 
    // if true then when camera report controller ready with new profile and stream config, 
    // system will not update profile and stream config to new one, but keep using old one, 
    // and update new profile and stream config to database, so that user can see the new profile and stream config in camera web page 
    // but system still use old profile and stream config until user change camera option to trigger controller recreate or manually update media profile config through api
    bool _keepConfigProfileAndStream = false;
    VideoEncoderConfig::VideoEncoderConfigMap _profileConfigMap; // current profile config map, used for checking whether media profile config is changed when camera report controller ready
    SdCardSyncConfig _sd_sync_config;
    // Tokens currently owned by a video config rule job, and whether each job's result was reported.
    VideoEncoderConfig::VideoEncoderConfigMap _ruleConfigMap;
    RuleReportContext::RuleReportContextMap _ruleReportMap;
};

} // namespace managerkit

#endif // CAMERA_CAMERACONTROLLER_H