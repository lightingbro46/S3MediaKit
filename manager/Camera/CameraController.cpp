#include "CameraController.h"
#include "Extension/Plugin.h"
#include "ext-plugin/onvif.h"
#include "Thread/WorkThreadPool.h"
#include "server/WebApiErrCode.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

CameraController::CameraController(const toolkit::EventPoller::Ptr &poller) : _poller(poller) {}

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

void CameraController::setupController(const CameraInfo &info, const CameraOption &option) {
    lock_guard<mutex> lck(_mtx_ctr);
    if (_controller) {
        DebugL << "Controller already exists: " << info.shortUrl() << ". Recreate controller due to configuration changed";
        _controller.reset();
    }

    // create new controller
    if (info.manufacturer.empty() || info.manufacturer == GENERIC_RTSP_CAMERA) {
        return;
    }

    if (info.ip.empty() || info.port == 0) {
        return;
    }

    string address = info.ip;
    if (option.autoWebPort) {
        address += ":" + (info.port > 0 ? to_string(info.port) : "80");
    } else {
        address += ":" + (option.webPort > 0 ? to_string(option.webPort) : "80");
    }

    // todo: create plugin from manufactor and model
    // todo: support PSI controller
    _controller = std::make_shared<OnvifController>(address, info.username, info.password);
    DebugL << "Created Onvif controller for device: " << info.shortUrl();

    _keep_remote_config = option.keepConfigProfileAndStream;
}

void CameraController::stopController() {  
    if (_on_ready) {
        auto enable_ptz = enablePTZ();
        _on_ready(false, "disconnected", enable_ptz);
        _on_ready = nullptr;
    }
    {
        lock_guard<mutex> lck(_mtx_ctr);
        _timer_ctr.reset();
        _controller.reset();
        _ready = false;
    }
}

void CameraController::onManager() {
    bool call_on_ready = false;
    {
        lock_guard<mutex> lck(_mtx_ctr);
        if (!_controller) {
            return;
        }

        if (!_ready && time(nullptr) - _last_reconnect_time >= 60) {
            auto ptr = dynamic_pointer_cast<OnvifController>(_controller);
            if (ptr) {
                // reconnect to device
                if (ptr->initControl()) {
                    _ready = true;
                    _err_msg = "connected";
                    InfoL << "Onvif controller " << ptr->getDeviceIp() << " connected";
                } else {
                    _ready = false;
                    _err_msg = ptr->getSoapErrMsg();
                    WarnL << "Onvif controller " << ptr->getDeviceIp() << " connect failed: " << _err_msg;
                }
                call_on_ready = true;
            }
            _last_reconnect_time = time(nullptr);
        }
    }

    // Call virtual method outside lock to prevent deadlock
    if (call_on_ready && _on_ready) {
        auto enable_ptz = enablePTZ();
        _on_ready(_ready, _err_msg, enable_ptz);
    }

    // WorkThreadPool::Instance().getPoller()->async([this]() {
    //     // Additional operations can be added here if controller is ready
    //     //todo: get media profile
    //     //todo: set media profile if need
    //     return 0;
    // });
}

static void onvifPTZMove(const OnvifController::Ptr &ptr, PTZ_DIRECT &direct, int &speed, const function<void(const SockException &ex)> &cb) {
    if (!ptr->enablePTZ()) {
        cb(SockException(Err_other, "Device do not support PTZ", ApiErrCode::CODE_DEVICE_NOT_SUPPORT_PTZ));
        return;
    }

    auto profile = ptr->getPTZProfile();
    auto clamp = [](float value, float minVal, float maxVal) { return std::max(minVal, std::min(maxVal, value)); };
    float step = speed / 100.0f;

    if (profile.isAbsMoveEnable) {
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

    if (profile.isRelMoveEnable) {
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
    
    if (profile.isConsMoveEnable) {
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
}

bool CameraController::enablePTZ() {
    bool enable_ptz = false;

    DeviceController::Ptr controller;
    {
        lock_guard<mutex> lck(_mtx_ctr);
        controller = _controller;
    }

    if (controller && _ready) {
        auto ptr = dynamic_pointer_cast<OnvifController>(controller);
        if (ptr) {
            enable_ptz = ptr->enablePTZ();
        }
    }

    return enable_ptz;
}

const std::string CameraController::getErrMsg() const {
    return _err_msg;
}

void CameraController::PTZMove(std::string &strDirect, int &speed, const function<void(const SockException &ex)> &cb) {
    // convert direction string to PTZ_DIRECT
    PTZ_DIRECT direct;
    if (strDirect == "up")
        direct = PTZ_DIRECT::Up;
    else if (strDirect == "down")
        direct = PTZ_DIRECT::Down;
    else if (strDirect == "right")
        direct = PTZ_DIRECT::Right;
    else if (strDirect == "left")
        direct = PTZ_DIRECT::Left;
    else if (strDirect == "zoomIn")
        direct = PTZ_DIRECT::ZoomIn;
    else if (strDirect == "zoomOut")
        direct = PTZ_DIRECT::ZoomOut;
    else
        direct = PTZ_DIRECT::Home;

    // find controller
    DeviceController::Ptr controller;
    {
        lock_guard<mutex> lck(_mtx_ctr);
        controller = _controller;
    }

    if (controller && _ready) {
        auto ptr = dynamic_pointer_cast<OnvifController>(controller);
        if (ptr) {
            onvifPTZMove(ptr, direct, speed, cb);
            return;
        }
        // todo: add more ptz function from manufacturer sdk
    }

    return cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
}

void CameraController::getMediaProfile() {

}

void CameraController::setMediaProfile() {

}

} // namespace managerkit