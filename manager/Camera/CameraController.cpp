#include "CameraController.h"
#include "Thread/WorkThreadPool.h"
#include "server/WebApiErrCode.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

CameraController::CameraController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller) : _tuple(tuple), _poller(poller) {}

void CameraController::start() {
    weak_ptr<CameraController> weak_self = shared_from_this();
    _timer_ctr = std::make_shared<Timer>(
        10.0f,
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

bool CameraController::isControlReady() const {
    return _ready.load(); 
}

void CameraController::setupController(const CameraOption &option) {
    lock_guard<mutex> lck(_mtx_ctr);
    if (_onvif_ctr) {
        // todo: check if the new option is different from current option, if not, skip recreate controller
        DebugL << "Controller of device: " << _tuple.shortUrl() << " already exists. Recreate controller due to configuration changed";
        _onvif_ctr.reset();
    }

    if (option.manufacturer == GENERIC_RTSP_CAMERA) {
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

    // save camera option for later use
    _address = address;
    _enablePTZControl = option.enablePTZControl;
    _reservePanAxis = option.reservePanAxis;
    _reserveTiltAxis = option.reserveTiltAxis;
    _ptzMode = option.ptzMode;
}

void CameraController::stopController() {  
    if (_on_ready) {
        _on_ready(false, "disconnected", nullptr);
        _on_ready = nullptr;
    }
    {
        lock_guard<mutex> lck(_mtx_ctr);
        _timer_ctr.reset();
        _onvif_ctr.reset();
        _ready = false;
    }
    DebugL << "Closed camera controller for device: " << _tuple.shortUrl() << " (" << _address << ")";
}

void CameraController::onManager() {
    bool call_on_ready = false;
    {
        lock_guard<mutex> lck(_mtx_ctr);
        if (!_onvif_ctr) {
            return;
        }

        if (!_ready && time(nullptr) - _last_reconnect_time >= 60) {
            if (_onvif_ctr) {
                // reconnect to device
                if (_onvif_ctr->connect()) {
                    _ready = true;
                    _err_msg = "connected";
                    InfoL << "Onvif controller of device " << _tuple.shortUrl() << " (" << _address << ") connected";

                    // get device capabilities after connected
                    _device_caps.isOnvifDevice = true;
                    _device_caps.onvifProfile.deviceInfo = _onvif_ctr->getDeviceInfo();
                    _device_caps.onvifProfile.mediaProfiles = _onvif_ctr->getMediaProfilesInfo();
                    _device_caps.onvifProfile.ptzProfile = _onvif_ctr->getPTZProfile();
                } else {
                    _ready = false;
                    _err_msg = _onvif_ctr->getSoapErrMsg();
                    WarnL << "Onvif controller of device " << _tuple.shortUrl() << " (" << _address << ") connect failed: " << _err_msg;
                }
                call_on_ready = true;
            }
            _last_reconnect_time = time(nullptr);
        }
    }

    // Call virtual method outside lock to prevent deadlock
    if (call_on_ready && _on_ready) {
        _on_ready(_ready.load(), _err_msg, &_device_caps);
    }

    // WorkThreadPool::Instance().getPoller()->async([this]() {
    //     // Additional operations can be added here if controller is ready
    //     //todo: get media profile
    //     //todo: set media profile if need
    //     return 0;
    // });
}

static void onvifPTZMove(const OnvifControl::Ptr &ptr, int ptz_mode, PTZ_DIRECT &direct, int &speed, const function<void(const SockException &ex)> &cb) {
    if (!ptr->enablePTZ()) {
        cb(SockException(Err_other, "Device does not support PTZ", ApiErrCode::CODE_DEVICE_NO_SUPPORT_PTZ));
        return;
    }

    auto profile = ptr->getPTZProfile();
    auto clamp = [](float value, float minVal, float maxVal) { return std::max(minVal, std::min(maxVal, value)); };
    float step = speed / 100.0f;

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
            case PTZ_DIRECT::Up: tilt = clamp(tilt * step, profile.relMinTilt, profile.relMaxTilt); break;
            case PTZ_DIRECT::Down: tilt = clamp(tilt * (-1) * step, profile.relMinTilt, profile.relMaxTilt); break;
            case PTZ_DIRECT::Left: pan = clamp(pan * (-1) * step, profile.relMinPan, profile.relMaxPan); break;
            case PTZ_DIRECT::Right: pan = clamp(pan * step, profile.relMinPan, profile.relMaxPan); break;
            case PTZ_DIRECT::ZoomIn: zoom = clamp(zoom * step, profile.relMinZoom, profile.relMaxZoom); break;
            case PTZ_DIRECT::ZoomOut: zoom = clamp(zoom * (-1) * step, profile.relMinZoom, profile.relMaxZoom); break;
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
            case PTZ_DIRECT::Up: tilt = clamp(tilt * step, profile.consMinTilt, profile.consMaxTilt); break;
            case PTZ_DIRECT::Down: tilt = clamp(tilt * (-1) * step, profile.consMinTilt, profile.consMaxTilt); break;
            case PTZ_DIRECT::Left: pan = clamp(pan * (-1) * step, profile.consMinPan, profile.consMaxPan); break;
            case PTZ_DIRECT::Right: pan = clamp(pan * step, profile.consMinPan, profile.consMaxPan); break;
            case PTZ_DIRECT::ZoomIn: zoom = clamp(zoom * step, profile.consMinZoom, profile.consMaxZoom); break;
            case PTZ_DIRECT::ZoomOut: zoom = clamp(zoom * (-1) * step, profile.consMinZoom, profile.consMaxZoom); break;
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

        EventPollerPool::Instance().getPoller()->doDelayTask(timeout_sec * 1000, [ptr, cb]() {
            if (!ptr->PTZ_Stop(true, true)) {
                cb(SockException(Err_other, "Device execute stop ptz continuous move failed: " + ptr->getSoapErrMsg(), ApiErrCode::CODE_PTZ_CONTINUOUS_CONTROL_FAILED));
                return 0;
            }
            // Execute success
            cb(SockException(Err_success, "Device execute ptz continuous move success", ApiErrCode::CODE_SUCCESS));
            return 0;
        });
    }
    /* Unreachable */
    cb(SockException(Err_other, "Device does not support selected PTZ control mode", ApiErrCode::CODE_DEVICE_NO_SUPPORT_SELECTED_PTZ_MODE));
}

bool CameraController::enablePTZ() {
    bool enable_ptz = false;
    OnvifControl::Ptr onvif_ptr;
    {
        lock_guard<mutex> lck(_mtx_ctr);
        onvif_ptr = _onvif_ctr;
    }

    if (onvif_ptr && _ready.load()) {
        enable_ptz = onvif_ptr->enablePTZ();
    }

    return enable_ptz;
}

const std::string CameraController::getErrMsg() const {
    return _err_msg;
}

void CameraController::PTZMove(std::string &strDirect, int &speed, const function<void(const SockException &ex)> &cb) {
    // check permission from camera option
    if (!_enablePTZControl) {
        cb(SockException(Err_other, "Camera is configured to disable PTZ control", ApiErrCode::CODE_DEVICE_CONFIG_DISABLE_PTZ));
        return;
    }

    // convert direction string to PTZ_DIRECT
    PTZ_DIRECT direct;
    if (strDirect == "up")
        direct = !_reservePanAxis ? PTZ_DIRECT::Up : PTZ_DIRECT::Down;
    else if (strDirect == "down")
        direct = !_reservePanAxis ? PTZ_DIRECT::Down : PTZ_DIRECT::Up;
    else if (strDirect == "right")
        direct = !_reserveTiltAxis ? PTZ_DIRECT::Right : PTZ_DIRECT::Left;
    else if (strDirect == "left")
        direct = !_reserveTiltAxis ? PTZ_DIRECT::Left : PTZ_DIRECT::Right;
    else if (strDirect == "zoomIn")
        direct = PTZ_DIRECT::ZoomIn;
    else if (strDirect == "zoomOut")
        direct = PTZ_DIRECT::ZoomOut;
    else
        direct = PTZ_DIRECT::Home;

    // find controller
    OnvifControl::Ptr onvif_ptr;
    {
        lock_guard<mutex> lck(_mtx_ctr);
        onvif_ptr = _onvif_ctr;
    }
    
    if (onvif_ptr && _ready.load()) {
        onvifPTZMove(onvif_ptr, _ptzMode, direct, speed, cb);
        return;
    }
    // todo: add more ptz function from manufacturer sdk

    return cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
}

void CameraController::getMediaProfile() {

}

void CameraController::setMediaProfile() {

}

} // namespace managerkit