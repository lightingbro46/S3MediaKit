#ifndef CAMERA_ONVIFCONTROL_H
#define CAMERA_ONVIFCONTROL_H

#include "GenericRtspCamera.h"
#include "ext-plugin/onvif.h"

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

    CameraController(const toolkit::EventPoller::Ptr &poller);

    ~CameraController() = default;

    void start();

    void setOnControllerReady(const OnControllerReady &cb) { _on_ready = std::move(cb); }

    bool isControlReady() const;

    void setupController(const CameraInfo &info, const CameraOption &option);

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
    toolkit::EventPoller::Ptr _poller;
    std::atomic<bool> _ready { false };
    std::string _err_msg;
    uint64_t _last_reconnect_time = 0;
    toolkit::Timer::Ptr _timer_ctr;
    OnvifController::Ptr _onvif_ctr;
    OnControllerReady _on_ready;
    DeviceCapabilities _device_caps;
    CameraInfo _info;
    CameraOption _option;
};

} // namespace managerkit

#endif // CAMERA_ONVIFCONTROL_H