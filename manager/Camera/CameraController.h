#ifndef CAMERA_ONVIFCONTROL_H
#define CAMERA_ONVIFCONTROL_H

#include "GenericRtspCamera.h"
#include "Common/DeviceController.h"

namespace managerkit {

enum class PTZ_DIRECT {
    Up = 1,
    Down,
    Left,
    Right,
    ZoomIn,
    ZoomOut,
    Home
};

class CameraController {
public:
    using Ptr = std::shared_ptr<CameraController>;

    CameraController(const CameraInfo &info);

    ~CameraController();

    bool isControlReady();

    void setupController(const CameraOption &option);

    void stopController();

    bool enablePTZ();
    
    void PTZMove(std::string &strDirect, int &speed, const std::function<void(const toolkit::SockException &ex)> &cb);

private:
    void onManager();

    virtual void onControllerReady() {};

    void getMediaProfile();

    void setMediaProfile();

private:
    std::recursive_mutex _mtx_control;
    CameraInfo _info;
    bool _controller_ready = false;
    uint64_t _last_reconnect_time = 0;
    toolkit::Timer::Ptr _timer_ctr;
    DeviceController::Ptr _controller;
};

} // namespace managerkit

#endif // CAMERA_ONVIFCONTROL_H