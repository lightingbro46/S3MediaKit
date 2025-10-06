#include "CameraController.h"
#include "Extension/Plugin.h"
#include "ext-plugin/onvif.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

CameraController::CameraController(const CameraInfo &info) : _info(info) {}

CameraController::~CameraController() {
    _timer_ctr.reset();
}

void CameraController::setupController() {
    lock_guard<recursive_mutex> lck(_mtx_control);
    if (_controller) {
        return;
    }

    // create new controller
    if (_info.manufacturer.empty() || _info.manufacturer == GENERIC_RTSP_CAMERA) {
        return;
    }

    if (_info.ip.empty() || _info.port == 0) {
        return;
    }

    string address = _info.ip;
    if (_info.port > 0) {
        address += ":" + to_string(_info.port);
    }
    // todo: create plugin from manufactor and model
    _controller = std::make_shared<OnvifController>(address, _info.username, _info.password);
    if (_controller->initControl()) {
        _controller_ready = true;
        InfoL << "Onvif controller " << _info.shortUrl() << " connected";
    } else {
        WarnL << "Onvif controller " << _info.shortUrl() << " connect failed";
    }

    _timer_ctr = std::make_shared<Timer>(
        10.0f,
        [&]() {
            onManager();
            return true;
        },
        nullptr
    );
}

void CameraController::stopController() {
    lock_guard<recursive_mutex> lck(_mtx_control);
    _controller_ready = false;
    _controller.reset();
    _timer_ctr.reset();
}

void CameraController::onManager() {
    lock_guard<recursive_mutex> lck(_mtx_control);
    if (!_controller) {
        return;
    }

    if (!_controller_ready) {
        // reconnect to get profile
        if (_controller->initControl()) {
            _controller_ready = true;
            InfoL << "Onvif controller " << _info.shortUrl() << " connected";
        }
    }

    //todo: get media profile
    //todo: set media profile if need

}

static void onvifPTZMove(const OnvifController::Ptr &ptr, PTZ_DIRECT &direct, int &speed, const function<void(const SockException &ex)> &cb) {
    //todo: only one session to control ptz
    if (!ptr->enablePTZ()) {
        cb(SockException(Err_other, "Device do not support PTZ"));
        return;
    }

    auto invoker = [cb](bool ret, string msg) {
        if (ret) {
            InfoL << "Execute PTZ control success: " << msg;
            cb(SockException(Err_success, msg));
        } else {
            WarnL << "Execute PTZ control failed: " << msg;
            cb(SockException(Err_other, msg));
        }
    };

    auto profile = ptr->getPTZProfile();
    auto clamp = [](float value, float minVal, float maxVal) { return std::max(minVal, std::min(maxVal, value)); };
    float step = speed / 100.0f;

    if (profile.isAbsMoveEnable) {
        float pan, tilt, zoom;
        auto status = ptr->PTZ_GetStatus(pan, tilt, zoom);
        if (status != tt__MoveStatus__IDLE) {
            invoker(false, "Device is running ptz control or unknown status: " + ptr->getSoapErrMsg());
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
            invoker(false, "Device execute ptz absolute move failed: " + ptr->getSoapErrMsg());
            return;
        }
        // Execute success
        return invoker(true, "Device execute ptz absolute move success");
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
            invoker(false, "Device execute ptz relative move failed: " + ptr->getSoapErrMsg());
            return;
        }
        // Execute success
        return invoker(true, "Device execute ptz relative move success");
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
            invoker(false, "Device execute ptz continuous move failed: " + ptr->getSoapErrMsg());
            return;
        }

        EventPollerPool::Instance().getPoller()->doDelayTask(timeout_sec * 1000, [ptr, invoker]() {
            if (!ptr->PTZ_Stop(true, true)) {
                invoker(false, "Device execute stop ptz continuous move failed: " + ptr->getSoapErrMsg());
                return 0;
            }
            // Execute success
            invoker(true, "Device execute ptz continuous move success");
            return 0;
        });
    }
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
    if (_controller && _controller_ready) {
        auto ptr = dynamic_pointer_cast<OnvifController>(_controller);
        if (ptr) {
            onvifPTZMove(ptr, direct, speed, cb);
            return;
        }
        // todo: add more ptz function from manufacturer sdk
    }

    return cb(SockException(Err_other, "Device do not support PTZ"));
}

void CameraController::getMediaProfile() {

}

void CameraController::setMediaProfile() {

}

} // namespace managerkit