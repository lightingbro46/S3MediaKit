#ifndef CAMERA_CAMERACONTROLLER_H
#define CAMERA_CAMERACONTROLLER_H

#include "GenericRtspCamera.h"
#include "Control/OnvifDeviceControl.h"

namespace managerkit {

struct OnvifProfile {
    std::vector<OnvifMediaProfile> mediaProfiles;
    OnvifPTZProfile ptzProfile;
    OnvifDeviceInfo deviceInfo;
};

struct DeviceCapabilities {
    bool isOnvifDevice = false;
    OnvifProfile onvifProfile;
};

class CameraController : public std::enable_shared_from_this<CameraController>  {
public:
    using Ptr = std::shared_ptr<CameraController>;
    using OnControllerReady = std::function<void(bool connect, const std::string &status, const DeviceCapabilities *caps)>;

    CameraController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller);

    ~CameraController() = default;

    void start();

    void setOnControllerReady(const OnControllerReady &cb) { _on_ready = std::move(cb); }

    bool isControlReady() const;

    void setupController(const CameraOption &option);

    void stopController();
    
    void PTZMove(std::string &strDirect, int &speed, const std::function<void(const toolkit::SockException &ex)> &cb);

private:
    bool enablePTZ();

    const std::string getErrMsg() const;

    void onManager();

    void getMediaProfile();

    void setMediaProfile();

private:
    std::mutex _mtx_ctr;
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    std::atomic<bool> _ready { false };
    std::string _err_msg;
    uint64_t _last_reconnect_time = 0;
    toolkit::Timer::Ptr _timer_ctr;
    OnvifControl::Ptr _onvif_ctr;
    OnControllerReady _on_ready;
    DeviceCapabilities _device_caps;
    std::string _address;
    int _ptzMode = CameraOption::kPTZModeAuto;
    bool _reservePanAxis = false;
    bool _reserveTiltAxis = false;
    bool _enablePTZControl = true;
};

} // namespace managerkit

#endif // CAMERA_CAMERACONTROLLER_H