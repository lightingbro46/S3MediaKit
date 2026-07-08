#ifndef CAMERA_CAMERACONTROLLER_H
#define CAMERA_CAMERACONTROLLER_H

#include "GenericRtspCamera.h"

namespace managerkit {

class CameraController : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<CameraController>  {
public:
    using Ptr = std::shared_ptr<CameraController>;

    CameraController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller);

    ~CameraController() = default;

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

private:
    void onManager();

    void onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps);

    void syncMediaProfile();

private:
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    std::atomic<bool> _ready { false };
    std::string _err_msg;
    uint64_t _last_reconnect_time = 0;
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
};

} // namespace managerkit

#endif // CAMERA_CAMERACONTROLLER_H