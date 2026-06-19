#include "CameraController.h"
#include "Thread/WorkThreadPool.h"
#include "server/WebApiErrCode.h"
#include "Util/onceToken.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

CameraController::CameraController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller) : _tuple(tuple), _poller(poller) {}

void CameraController::createTimer() {
    weak_ptr<CameraController> weak_self = shared_from_this();
    _timer_ctr = std::make_shared<Timer>(
        30.0f,
        [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            strong_self->onManager();
            return true;
        },
        _poller
    );
}

void CameraController::setListener(const std::shared_ptr<DeviceSourceEvent> &listener) {
    setDelegate(listener);
}

bool CameraController::isReady() const {
    return _ready.load(); 
}

void CameraController::setupController(const CameraOption &option) {
    // Caller: GenericRtspCameraImp::setupController() via setCameraOption() which asserts isCurrentThread.
    CHECK(_poller->isCurrentThread(), "setupController must be called on the owner poller");

    if (_onvif_ctr) {
        if (ControllerOption::from(option) == _ctrl_option) {
            // Connection unchanged — update PTZ settings in-place, no reconnect needed
            _enablePTZControl = option.enablePTZControl;
            _reversePanAxis   = option.reversePanAxis;
            _reverseTiltAxis  = option.reverseTiltAxis;
            _ptzMode          = option.ptzMode;
            _ptzSpeed         = option.ptzSpeed;
            _keepConfigProfileAndStream = option.keepConfigProfileAndStream;
            DebugL << "Controller of device: " << _tuple.shortUrl() << " connection unchanged, updated PTZ settings in-place";
            return;
        }
        DebugL << "Controller of device: " << _tuple.shortUrl() << " connection changed, recreating controller";
        _onvif_ctr.reset();
        _ready = false;
    }

    if (option.manufacturer == GENERIC_RTSP_CAMERA && option.ip.empty()) {
        WarnL << "Generic RTSP camera controller is not implemented yet";
        return;
    }

    if (option.ip.empty() || option.port == 0) {
        WarnL << "Invalid camera address, ip or port is empty";
        return;
    }

    string address = option.ip;
    if (option.autoWebPort) {
        address += ":" + (option.port > 0 ? to_string(option.port) : "80");
    } else {
        address += ":" + (option.webPort > 0 ? to_string(option.webPort) : "80");
    }

    // create new controller, currently only support onvif controller
    // todo: create plugin from manufactor and model
    // todo: support PSI controller
    _onvif_ctr = std::make_shared<OnvifControl>(address, option.username, option.password);
    DebugL << "Created Onvif controller for device: " << _tuple.shortUrl() << " (" << address << ")"
        << ", username: " << (option.username.empty() ? "empty" : "******")
        << ", password: " << (option.password.empty() ? "empty" : "******");

    // save controller option and PTZ settings for later use
    _address      = address;
    _ctrl_option  = ControllerOption::from(option);
    _enablePTZControl = option.enablePTZControl;
    _reversePanAxis   = option.reversePanAxis;
    _reverseTiltAxis  = option.reverseTiltAxis;
    _ptzMode          = option.ptzMode;
    _ptzSpeed         = option.ptzSpeed;
    _keepConfigProfileAndStream = option.keepConfigProfileAndStream;
}

void CameraController::stopController() {  
    CHECK(_poller->isCurrentThread(), "stopController must be called on the owner poller");

    onControllerReady(false, "Stop", nullptr);
    
    _timer_ctr.reset();
    _onvif_ctr.reset();
    _ready = false;
    DebugL << "Closed camera controller for device: " << _tuple.shortUrl() << " (" << _address << ")";
}

void CameraController::onManager() {
    // Always called from the Timer which runs on _poller - no dispatch needed.
    if (!_onvif_ctr) {
        onControllerReady(false, "Not supported", std::make_shared<DeviceCapabilities>());
        return;
    }

    if (time(nullptr) - _last_reconnect_time < 60 && _isControlled.load()) {
        // avoid reconnecting too frequently
        return;
    }
    _last_reconnect_time = time(nullptr);

    auto onvif_ctr = _onvif_ctr;
    auto weak_self = weak_from_this();
    // ONVIF connect() is a blocking SOAP/HTTP call — dispatch on WorkThreadPool,
    // not EventPollerPool, to avoid blocking stream I/O pollers.
    WorkThreadPool::Instance().getPoller()->async([weak_self, onvif_ctr]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        auto tuple = strong_self->_tuple;
        auto address = strong_self->_address;
        // reconnect to device
        if (onvif_ctr->connect()) {
            InfoL << "Onvif controller of device " << tuple.shortUrl() << " (" << address << ") connected";
            // set camera time manual
            onvif_ctr->setCameraTimeManual();
            strong_self->syncMediaProfile();
            
            // get device capabilities after connected
            auto caps = std::make_shared<DeviceCapabilities>();
            caps->isOnvifDevice = true;
            caps->onvifProfile.deviceInfo = onvif_ctr->getDeviceInfo();
            caps->onvifProfile.mediaProfiles = onvif_ctr->getMediaProfilesInfo();
            caps->onvifProfile.ptzProfile = onvif_ctr->getPTZProfile();

            strong_self->onControllerReady(true, "connected", std::move(caps));
        } else {
            auto err_msg = onvif_ctr->getSoapErrMsg();
            WarnL << "Onvif controller of device " << tuple.shortUrl() << " (" << address << ") connect failed: " << err_msg;

            strong_self->onControllerReady(false, err_msg, nullptr);
        }
    });

    // Additional operations can be added here if controller is ready
    //todo: get media profile
    //todo: set media profile if need
    //todo: set user defined presets if need
}

static void onvifPTZMove(const OnvifControl::Ptr &ptr, int ptz_mode, PTZ_DIRECT direct, int speed, const function<void(const SockException &ex)> &cb) {
    if (!ptr->enablePTZ()) {
        cb(SockException(Err_other, "Device does not support PTZ", ApiErrCode::CODE_DEVICE_NO_SUPPORT_PTZ));
        return;
    }

    auto profile = ptr->getPTZProfile();
    auto clamp = [](float value, float minVal, float maxVal) { return std::max(minVal, std::min(maxVal, value)); };
    float step = speed / 100.0f;

    if (direct == PTZ_DIRECT::Home) {
        // If Home command and preset is enabled, go to home preset if exist, otherwise go to home position
        if (!ptr->PTZ_GotoHomePosition(step, step, step)) {
            cb(SockException(Err_other, "Device execute ptz goto home position failed: " + ptr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_GOTO_HOME_FAILED));
            return;
        }
        return cb(SockException(Err_success, "Device execute ptz go to home success", ApiErrCode::CODE_SUCCESS));
    }

    if (profile.isAbsMoveEnable && (ptz_mode == CameraOption::kPTZAbsolutedMode || ptz_mode == CameraOption::kPTZModeAuto)) {
        float pan, tilt, zoom;
        auto status = ptr->PTZ_GetStatus(pan, tilt, zoom);
        if (status != tt__MoveStatus__IDLE) {
            auto err_msg = status == tt__MoveStatus__MOVING ? "Device is running ptz control" : ("Device ptz status unknown: " + ptr->getSoapErrMsg());
            cb(SockException(Err_other, err_msg, ApiErrCode::CODE_DEVICE_IS_RUNNING_PTZ));
            return;
        }
        switch (direct) {
            case PTZ_DIRECT::Up: tilt = clamp(tilt + step, profile.absMinTilt, profile.absMaxTilt); break;
            case PTZ_DIRECT::Down: tilt = clamp(tilt - step, profile.absMinTilt, profile.absMaxTilt); break;
            case PTZ_DIRECT::Left: pan = clamp(pan - step, profile.absMinPan, profile.absMaxPan); break;
            case PTZ_DIRECT::Right: pan = clamp(pan + step, profile.absMinPan, profile.absMaxPan); break;
            case PTZ_DIRECT::ZoomIn: zoom = clamp(zoom + step, profile.absMinZoom, profile.absMaxZoom); break;
            case PTZ_DIRECT::ZoomOut: zoom = clamp(zoom - step, profile.absMinZoom, profile.absMaxZoom); break;
            case PTZ_DIRECT::Home:
                pan = (profile.absMinPan + profile.absMaxPan) / 2;
                tilt = (profile.absMinTilt + profile.absMaxTilt) / 2;
                zoom = (profile.absMinZoom + profile.absMaxZoom) / 2;
                break;
            default: break;
        }
        if (!ptr->PTZ_AbsoluteMove(pan, tilt, zoom, step, step, step)) {
            cb(SockException(Err_other, "Device execute ptz absolute move failed: " + ptr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_ABSOLUTED_CONTROL_FAILED));
            return;
        }
        // Execute success
        return cb(SockException(Err_success, "Device execute ptz absolute move success", ApiErrCode::CODE_SUCCESS));
    };

    if (profile.isRelMoveEnable && (ptz_mode == CameraOption::kPTZRelativeMode || ptz_mode == CameraOption::kPTZModeAuto)) {
        float pan = 0.0, tilt = 0.0, zoom = 0.0;
        switch (direct) {
            case PTZ_DIRECT::Up: tilt = clamp(step, profile.relMinTilt, profile.relMaxTilt); break;
            case PTZ_DIRECT::Down: tilt = clamp(-step, profile.relMinTilt, profile.relMaxTilt); break;
            case PTZ_DIRECT::Left: pan = clamp(-step, profile.relMinPan, profile.relMaxPan); break;
            case PTZ_DIRECT::Right: pan = clamp(step, profile.relMinPan, profile.relMaxPan); break;
            case PTZ_DIRECT::ZoomIn: zoom = clamp(step, profile.relMinZoom, profile.relMaxZoom); break;
            case PTZ_DIRECT::ZoomOut: zoom = clamp(-step, profile.relMinZoom, profile.relMaxZoom); break;
            case PTZ_DIRECT::Home:
                pan = (profile.relMinPan + profile.relMaxPan) / 2;
                tilt = (profile.relMinTilt + profile.relMaxTilt) / 2;
                zoom = (profile.relMinZoom + profile.relMaxZoom) / 2;
                break;
            default: break;
        }
        if (!ptr->PTZ_RelativeMove(pan, tilt, zoom , step, step, step)) {
            cb(SockException(Err_other, "Device execute ptz relative move failed: " + ptr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_RELATIVE_CONTROL_FAILED));
            return;
        }
        // Execute success
        return cb(SockException(Err_success, "Device execute ptz relative move success", ApiErrCode::CODE_SUCCESS));
    } 
    
    if (profile.isConsMoveEnable && (ptz_mode == CameraOption::kPTZContinousMode || ptz_mode == CameraOption::kPTZModeAuto)) {
        float pan = 0.0, tilt = 0.0, zoom = 0.0;
        switch (direct) {
            case PTZ_DIRECT::Up: tilt = clamp(step, profile.consMinTilt, profile.consMaxTilt); break;
            case PTZ_DIRECT::Down: tilt = clamp(-step, profile.consMinTilt, profile.consMaxTilt); break;
            case PTZ_DIRECT::Left: pan = clamp(-step, profile.consMinPan, profile.consMaxPan); break;
            case PTZ_DIRECT::Right: pan = clamp(step, profile.consMinPan, profile.consMaxPan); break;
            case PTZ_DIRECT::ZoomIn: zoom = clamp(step, profile.consMinZoom, profile.consMaxZoom); break;
            case PTZ_DIRECT::ZoomOut: zoom = clamp(-step, profile.consMinZoom, profile.consMaxZoom); break;
            case PTZ_DIRECT::Home:
                pan = (profile.consMinPan + profile.consMaxPan) / 2;
                tilt = (profile.consMinTilt + profile.consMaxTilt) / 2;
                zoom = (profile.consMinZoom + profile.consMaxZoom) / 2;
                break;
            default: break;
        }

        int timeout_sec = 1;
        if (!ptr->PTZ_ContinuousMove(pan, tilt, zoom, timeout_sec)) {
            cb(SockException(Err_other, "Device execute ptz continuous move failed: " + ptr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_CONTINUOUS_CONTROL_FAILED));
            return;
        }

        // PTZ_Stop() is a blocking SOAP call — schedule on WorkThreadPool, not EventPollerPool.
        WorkThreadPool::Instance().getPoller()->doDelayTask(timeout_sec * 1000, [ptr, cb]() {
            if (!ptr->PTZ_Stop(true, true)) {
                cb(SockException(Err_other, "Device execute stop ptz continuous move failed: " + ptr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_CONTINUOUS_CONTROL_FAILED));
                return 0;
            }
            // Execute success
            cb(SockException(Err_success, "Device execute ptz continuous move success", ApiErrCode::CODE_SUCCESS));
            return 0;
        });
        return;
    }
    /* Unreachable */
    cb(SockException(Err_other, "Device does not support selected PTZ control mode", ApiErrCode::CODE_DEVICE_NO_SUPPORT_SELECTED_PTZ_MODE));
}

void CameraController::PTZMove(const std::string &strDirect, int speed, const function<void(const SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::PTZMove) asserts isCurrentThread - no dispatch needed.
    // check permission from camera option
    if (!_enablePTZControl) {
        cb(SockException(Err_other, "Camera is configured to disable PTZ control", ApiErrCode::CODE_DEVICE_CONFIG_DISABLE_PTZ));
        return;
    }

    // convert direction string to PTZ_DIRECT
    PTZ_DIRECT direct;
    if (strDirect == "up")
        direct = !_reverseTiltAxis ? PTZ_DIRECT::Up : PTZ_DIRECT::Down;
    else if (strDirect == "down")
        direct = !_reverseTiltAxis ? PTZ_DIRECT::Down : PTZ_DIRECT::Up;
    else if (strDirect == "right")
        direct = !_reversePanAxis ? PTZ_DIRECT::Right : PTZ_DIRECT::Left;
    else if (strDirect == "left")
        direct = !_reversePanAxis ? PTZ_DIRECT::Left : PTZ_DIRECT::Right;
    else if (strDirect == "zoomIn")
        direct = PTZ_DIRECT::ZoomIn;
    else if (strDirect == "zoomOut")
        direct = PTZ_DIRECT::ZoomOut;
    else
        direct = PTZ_DIRECT::Home;

    int ptz_speed = speed < 0 ? speed : static_cast<int>(_ptzSpeed);

    if (_onvif_ctr && _ready.load()) {
        _isControlled = true;
        // Read state on _poller before dispatching to avoid data races.
        auto onvif_ctr = _onvif_ctr;
        int ptz_mode   = _ptzMode;
        std::weak_ptr<CameraController> weak_self = shared_from_this();
        // Blocking SOAP — dispatch to a fresh WorkThread so _poller stays responsive.
        WorkThreadPool::Instance().getPoller()->async([weak_self, onvif_ctr, ptz_mode, direct, ptz_speed, cb]() {
            onvifPTZMove(onvif_ctr, ptz_mode, direct, ptz_speed, cb);
            if (auto self = weak_self.lock()) { self->_isControlled = false; }
        });
        return;
    }
    // todo: add more ptz function from manufacturer sdk

    return cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
}

void CameraController::onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps) {
    if (!_poller->isCurrentThread()) {
        auto self = shared_from_this();
        _poller->async([self, connect, status, caps]() {
            self->onControllerReady(connect, status, caps);
        });
        return;
    }

    _ready = connect;
    _err_msg = status;
    auto data = toolkit::Any(caps);
    DeviceSourceEventInterceptor::onControllerReady(DeviceSource::NullDeviceSource(), connect, status, data);
}

bool CameraController::addUserPTZPreset(const std::string &presetToken, const std::string &presetName, float &pan, float &tilt, float &zoom, const std::function<void(const toolkit::SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::addUserPTZPreset) asserts isCurrentThread - no dispatch needed.
    if (presetToken.empty() || presetName.empty()) {
        return false;
    }
    if (_onvif_ctr && _ready.load()) {
        onceToken token1([&] {_isControlled = true;}, [&]() { _isControlled = false; });
        auto profile = _onvif_ctr->getPTZProfile();

        if (!profile.isPresetEnable) {
            cb(SockException(Err_other, "Device does not support PTZ preset", ApiErrCode::CODE_DEVICE_NO_SUPPORT_PTZ_PRESET));
            return false;
        }

        if (!_onvif_ctr->PTZ_GetStatus(pan, tilt, zoom)) {
            WarnL << "Failed to get current PTZ status from device: " << _tuple.shortUrl() << ", error: " << _onvif_ctr->getSoapErrMsg();
            cb(SockException(Err_other, "Execute ptz get status failed: " + _onvif_ctr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_GET_STATUS_FAILED));
            return false;
        }

        addUserPTZPreset(presetToken, presetName, pan, tilt, zoom);
        cb(SockException(Err_success, "User preset added successfully", ApiErrCode::CODE_SUCCESS));
        return true;
    }

    cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
    return false;
}

bool CameraController::addUserPTZPreset(const std::string &presetToken, const std::string &presetName, float &pan, float &tilt, float &zoom) {
    OnvifPTZProfile::PTZPreset preset;
    preset.Token = presetToken;
    preset.Name = presetName;
    preset.absPan = pan;
    preset.absTilt = tilt;
    preset.absZoom = zoom;
    _userPresets[presetToken] = std::move(preset);
    return true;
}

bool CameraController::removeUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::removeUserPTZPreset) asserts isCurrentThread - no dispatch needed.
    if (_userPresets.find(presetToken) != _userPresets.end()) {
        _userPresets.erase(presetToken);
        cb(SockException(Err_success, "User preset removed successfully", ApiErrCode::CODE_SUCCESS));
        return true;
    }
    cb(SockException(Err_other, "User preset not found", ApiErrCode::CODE_PTZ_PRESET_NOT_FOUND));
    return false;
}

void CameraController::PTZGotoPreset(const std::string &presetToken, bool isUserPreset, const std::function<void(const toolkit::SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::PTZGotoPreset) asserts isCurrentThread - no dispatch needed.
    // check permission from camera option
    if (!_enablePTZControl) {
        cb(SockException(Err_other, "Camera is configured to disable PTZ control", ApiErrCode::CODE_DEVICE_CONFIG_DISABLE_PTZ));
        return;
    }
    _isControlled = true;
    std::weak_ptr<CameraController> weak_self = shared_from_this();

    if (isUserPreset) {
        auto it = _userPresets.find(presetToken);
        if (it == _userPresets.end()) {
            _isControlled = false;
            cb(SockException(Err_other, "User preset not found", ApiErrCode::CODE_PTZ_PRESET_NOT_FOUND));
            return;
        }
        auto &preset = it->second;
        if (_onvif_ctr && _ready.load()) {
            auto onvif_ctr = _onvif_ctr;
            float pan = preset.absPan, tilt = preset.absTilt, zoom = preset.absZoom;
            // Blocking SOAP — dispatch to fresh WorkThread so _poller stays responsive.
            WorkThreadPool::Instance().getPoller()->async([weak_self, onvif_ctr, pan, tilt, zoom, cb]() mutable {
                if (!onvif_ctr->PTZ_AbsoluteMove(pan, tilt, zoom)) {
                    cb(SockException(Err_other, "Device execute ptz goto user preset failed: " + onvif_ctr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_GOTO_USER_PRESET_FAILED));
                } else {
                    cb(SockException(Err_success, "Device execute ptz goto user preset success", ApiErrCode::CODE_SUCCESS));
                }
                if (auto self = weak_self.lock()) { self->_isControlled = false; }
            });
            return;
        } else {
            _isControlled = false;
            cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
            return;
        }
    } else {
        if (_onvif_ctr && _ready.load()) {
            auto onvif_ctr = _onvif_ctr;
            string token = presetToken;
            // Blocking SOAP — dispatch to fresh WorkThread so _poller stays responsive.
            WorkThreadPool::Instance().getPoller()->async([weak_self, onvif_ctr, token, cb]() {
                if (!onvif_ctr->PTZ_GotoPreset(token, 0.5, 0.5, 0.5)) {
                    cb(SockException(Err_other, "Device execute ptz goto preset failed: " + onvif_ctr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_GOTO_PRESET_FAILED));
                } else {
                    cb(SockException(Err_success, "Device execute ptz goto preset success", ApiErrCode::CODE_SUCCESS));
                }
                if (auto self = weak_self.lock()) { self->_isControlled = false; }
            });
            return;
        } else {
            _isControlled = false;
            cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
            return;
        }
    }
}

static void onvifGetMediaProfile(const OnvifControl::Ptr &ptr, const string &profileToken, const std::function<void(const toolkit::SockException &ex, VideoEncoderConfig &config)> &cb) {
    VideoEncoderConfig config;
    if (!ptr->getVideoEncoderConfigByToken(profileToken, config)) {
        cb(SockException(Err_other, "Device execute get media profile failed: " + ptr->getSoapErrMsg(), ApiErrCode::CODE_ONVIF_GET_CONFIG_FAILED), config);
        return;
    }
    cb(SockException(Err_success, "Device execute get media profile success", ApiErrCode::CODE_SUCCESS), config);
}

void CameraController::getMediaProfileAsync(const string &profileToken, const std::function<void(const toolkit::SockException &ex, VideoEncoderConfig &config)> &cb) {
    // Caller (GenericRtspCameraImp::getMediaProfile) asserts isCurrentThread - no dispatch needed.
    if (_onvif_ctr && _ready.load()) {
        _isControlled = true;
        auto onvif_ctr = _onvif_ctr;
        auto poller = _poller;
        string token = profileToken;
        std::weak_ptr<CameraController> weak_self = shared_from_this();
        auto on_callback = [weak_self, poller, cb](const toolkit::SockException &ex, VideoEncoderConfig &config) mutable {
            VideoEncoderConfig config_copy = config; // Make a copy to avoid dangling reference
            poller->async([weak_self, cb, ex, config_copy]() mutable {
                if (auto self = weak_self.lock()) {
                    self->_isControlled = false;
                }
                cb(ex, config_copy);
            });
        };
        WorkThreadPool::Instance().getPoller()->async([weak_self, onvif_ctr, token, on_callback]() {
            onvifGetMediaProfile(onvif_ctr, token, on_callback);
        });
        return;
    }
    // todo: add more ptz function from manufacturer sdk
    VideoEncoderConfig config;
    return cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE), config);
}

static void onvifSetMediaProfile(const OnvifControl::Ptr &ptr, const string &profileToken, const VideoEncoderConfig &config, const function<void(const SockException &ex)> &cb, const function<void()> &on_success) { 
    if (!ptr->setVideoEncoderConfigByToken(profileToken, config)) {
        cb(SockException(Err_other, "Device execute set media profile failed: " + ptr->getSoapErrMsg(), ApiErrCode::CODE_ONVIF_SET_CONFIG_FAILED));
        return;
    }
        
    on_success();
    return cb(SockException(Err_success, "Device execute set media profile success", ApiErrCode::CODE_SUCCESS));
}

void CameraController::setMediaProfileAsync(const string &profileToken, VideoEncoderConfig &config, const function<void(const SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::setMediaProfile) asserts isCurrentThread - no dispatch needed.
    if (_onvif_ctr && _ready.load()) {
        _isControlled = true;

        VideoEncoderConfig new_f_config;
        auto it = _profileConfigMap.find(profileToken);
        if (it != _profileConfigMap.end()) {
            VideoEncoderConfig f_config = it->second;
            
            new_f_config.vcodec = config.vcodec;
            new_f_config.fps = config.fps;
            new_f_config.bitrate = config.bitrate;
            new_f_config.width = config.width;
            new_f_config.height = config.height;
            new_f_config.state.retry_time = f_config.state.retry_time;
            new_f_config.state.status = f_config.state.status;
            if (config != f_config) {
                TraceL << "Profile config changed by setting";
                new_f_config.state.retry_time = 5;
                new_f_config.state.status = configStateToString[VideoConfigSetState::NEW];
            } else {
                TraceL << "The config does not change";
                _isControlled = false;
                return cb(SockException(Err_other, "New config is the same as the current config", ApiErrCode::CODE_ONVIF_SET_CONFIG_NOT_CHANGE));
            }
        } else {
            TraceL << "Profile config not exist in current map, add new config";
            new_f_config = config;
            new_f_config.state.retry_time = 5;
            new_f_config.state.status = configStateToString[VideoConfigSetState::NEW];
            addProfileConfig(profileToken, new_f_config);
        }
        auto onvif_ctr = _onvif_ctr;
        auto poller = _poller;
        std::weak_ptr<CameraController> weak_self = shared_from_this();
        auto on_success = [profileToken, new_f_config, poller, weak_self]() mutable {
            poller->async([weak_self, profileToken, new_f_config]() mutable {
                if (auto self = weak_self.lock()) {
                    self->_isControlled = false;
                    self->addProfileConfig(profileToken, new_f_config);
                }
            });
        };
        WorkThreadPool::Instance().getPoller()->async([weak_self, onvif_ctr, profileToken, config, cb, on_success]() {
            onvifSetMediaProfile(onvif_ctr, profileToken, config, cb, on_success);
        });
        return;
    }    
    // todo: add more ptz function from manufacturer sdk

    return cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
}

void CameraController::addProfileConfig(const std::string &profileToken, const VideoEncoderConfig &config, bool emitEvent) {
    DebugL << "Add profile config to map, profile token: " << profileToken 
        << ", vcodec: " << config.vcodec 
        << ", fps: " << config.fps 
        << ", bitrate: " << config.bitrate 
        << ", width: " << config.width 
        << ", height: " << config.height
        << ", retry_time: " << config.state.retry_time
        << ", status: " << config.state.status;
    // Update the profile config in the map and emit event if needed. This function is called after successfully set media profile to camera or retry setting
    _profileConfigMap[profileToken] = config;

    if (emitEvent) {
        auto flag = NOTICE_EMIT(BroadcastStreamSettingChangeArgs, Broadcast::kBroadcastStreamSettingChange, _tuple, profileToken, config);
        if (!flag) {
            WarnL << "Nobody listen for stream change event";
        }
    }
}

void CameraController::syncMediaProfile() {
    // This function is called by timer to retry setting media profile for profiles in NEW state. 
    // It does not need to check current state before setting because the state will be updated after setMediaProfileAsync called, 
    // and the retry logic is based on retry_time which will be reset to 5 after each try.
    if (_onvif_ctr && _ready.load()) {
        auto current_profiles = _onvif_ctr->getMediaProfilesInfo();
        for (const auto &profile : current_profiles) {
            auto token = profile.token;
            auto it = _profileConfigMap.find(token);
            if (it == _profileConfigMap.end()) {
                TraceL << "Profile token: " << token << " not found in current profile config map, skip syncing";
                continue;
            }

            VideoEncoderConfig f_config = it->second;
            if (profile.isVideoConfigEqual(f_config)) {
                f_config.state.retry_time = 5;
                if (f_config.state.status != configStateToString[VideoConfigSetState::EXTERNAL]) {
                    f_config.state.status = configStateToString[VideoConfigSetState::SUCCESS];
                    InfoL << "Profile token: " << token << " config is consistent with camera, mark as SUCCESS";
                }
            } else {
                // The config in camera is different from the config in map, which means the config is changed by camera or failed to set to camera. 
                // We will retry setting config to camera if retry_time > 0, otherwise we will consider it as failed and update the state to FAILED. 
                // If the config is changed by camera, we will update the config in map to keep it consistent with camera, and set the state to EXTERNAL 
                // to indicate the config is changed by external and we will not try to set it to camera until next time when we detect the config is changed again.
                if (f_config.state.retry_time == 0 && f_config.state.status == configStateToString[VideoConfigSetState::PROCESSING]) {
                    f_config.state.retry_time = 0;
                    f_config.state.status = configStateToString[VideoConfigSetState::FAILED];
                    InfoL << "Profile token: " << token << " config failed to set to camera after retrying, mark as FAILED";
                } else if (f_config.state.retry_time > 0 && (f_config.state.status == configStateToString[VideoConfigSetState::PROCESSING] || f_config.state.status == configStateToString[VideoConfigSetState::NEW]) ) {
                    if (_onvif_ctr->setVideoEncoderConfigByToken(token, f_config)) {
                        f_config.state.retry_time--;
                        f_config.state.status = configStateToString[VideoConfigSetState::PROCESSING];
                        InfoL << "Retry set config to camera; retries left: " << f_config.state.retry_time;
                    } else {
                        f_config.state.retry_time = 0;
                        f_config.state.status = configStateToString[VideoConfigSetState::FAILED];
                        WarnL << "Failed to retry set config to camera: " << _onvif_ctr->getSoapErrMsg();
                    }
                } else {
                    InfoL << "Profile token: " << token << " config changed by camera";
                    // If the previous config update failed after 5 retry attempts,
                    // update the user with the current camera config instead of
                    // retrying the same failed config again.
                    if (!_keepConfigProfileAndStream) {
                        f_config.state.retry_time = 4;
                        f_config.state.status = configStateToString[VideoConfigSetState::PROCESSING];
                        if (!_onvif_ctr->setVideoEncoderConfigByToken(token, f_config)) {
                            WarnL << "Failed to set new config to camera: " << _onvif_ctr->getSoapErrMsg();
                        }
                    } else {
                        f_config.vcodec = profile.vcodec;
                        f_config.fps = profile.fps;
                        f_config.bitrate = profile.bitrate;
                        f_config.width = profile.width;
                        f_config.height = profile.height;
                        f_config.state.retry_time = 5;
                        f_config.state.status = configStateToString[VideoConfigSetState::EXTERNAL];
                        InfoL << "Keep the config in map consistent with camera for profile token: " << token;
                    }
                }
            }
            addProfileConfig(token, f_config);
        }
    } else {
        WarnL << "Device controller is not ready when retrying set media profile";
    }
}

} // namespace managerkit