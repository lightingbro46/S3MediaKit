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

private:
    void onManager();

    void getMediaProfile();

    void setMediaProfile();

    void onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps);

private:
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    std::atomic<bool> _ready { false };
    std::string _err_msg;
    uint64_t _last_reconnect_time = 0;
    toolkit::Timer::Ptr _timer_ctr;
    OnvifControl::Ptr _onvif_ctr;
    std::string _address;
    int _ptzMode = CameraOption::kPTZModeAuto;
    bool _reservePanAxis = false;
    bool _reserveTiltAxis = false;
    bool _enablePTZControl = true;
};

} // namespace managerkit

#endif // CAMERA_CAMERACONTROLLER_H