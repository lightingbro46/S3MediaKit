#ifndef CAMERA_ONVIFCONTROL_H
#define CAMERA_ONVIFCONTROL_H

#include "GenericRtspCamera.h"
#include "Common/DeviceController.h"

namespace managerkit {

class CameraController : public std::enable_shared_from_this<CameraController>  {
public:
    using Ptr = std::shared_ptr<CameraController>;
    using OnControllerReady = std::function<void(bool enablePTZ)>;

    CameraController(const toolkit::EventPoller::Ptr &poller);

    ~CameraController();

    void start();

    void setOnControllerReady(const OnControllerReady &cb) { _on_ready = std::move(cb); }

    bool isControlReady() const;

    void setupController(const CameraInfo &info, const CameraOption &option);

    void stopController();
    
    void PTZMove(std::string &strDirect, int &speed, const std::function<void(const toolkit::SockException &ex)> &cb);

private:
    bool enablePTZ();

    void onManager();

    void getMediaProfile();

    void setMediaProfile();

private:
    std::mutex _mtx_ctr;
    toolkit::EventPoller::Ptr _poller;
    std::atomic<bool> _ready { false };
    uint64_t _last_reconnect_time = 0;
    toolkit::Timer::Ptr _timer_ctr;
    DeviceController::Ptr _controller;
    OnControllerReady _on_ready;
    
};

} // namespace managerkit

#endif // CAMERA_ONVIFCONTROL_H