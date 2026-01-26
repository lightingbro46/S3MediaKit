#ifndef S3PLUGINKIT_ONVIFCONTROL_H
#define S3PLUGINKIT_ONVIFCONTROL_H

#include <map>
#include "soapDeviceBindingProxy.h"
#include "soapMediaBindingProxy.h"
#include "soapImagingBindingProxy.h"
#include "soapPTZBindingProxy.h"
#include "Common/DeviceController.h"

namespace managerkit {

struct OnvifDeviceInfo {
    std::string manufacturer;
    std::string model;
    std::string firmwareVersion;
    std::string serialNumber;
    std::string hardwareId;
    std::string macAddress;
};

struct OnvifPTZProfile {
    std::string strMediaProfileToken;
    bool isAbsMoveEnable = false;
    float absMinPan = -1;
    float absMaxPan = 1;
    float absMinTilt = -1;
    float absMaxTilt = 1;
    float absMinZoom = 0;
    float absMaxZoom = 1;

    bool isConsMoveEnable = false;
    float consMinPan = -1;
    float consMaxPan = 1;
    float consMinTilt = -1;
    float consMaxTilt = 1;
    float consMinZoom = -1;
    float consMaxZoom = 1;

    bool isRelMoveEnable = false;
    float relMinPan = -1;
    float relMaxPan = 1;
    float relMinTilt = -1;
    float relMaxTilt = 1;
    float relMinZoom = -1;
    float relMaxZoom = 1;
};

struct OnvifMediaProfile {
    std::string token;
    std::string url;
    bool hasVideo = false;
    std::string vcodec;
    int width;
    int height;
    int bitrate;
    float fps;
    float quality;
    bool hasAudio = false;
    std::string acodec;
    int channelNo;
    std::string sampleRate;
    std::string sampleBit;

    struct VideoConfigOption {
        std::pair<int,int> FrameRateRange;
        std::vector<std::pair<int,int> > ResAvailable;
        std::pair<int,int> BitRateRange;
        std::pair<int,int> QualityRange;
    };
    VideoConfigOption vOption;
};

class OnvifController : public DeviceController {
public:
    using Ptr = std::shared_ptr<OnvifController>;
    using OnvifMediaProfileMap = std::vector<OnvifMediaProfile>;

    OnvifController(std::string strDeviceIp, std::string strUsername = "", std::string strPassword = "");
    ~OnvifController() override;

    bool initControl() override;

    /**
     * Get onvif device info
     */
    OnvifDeviceInfo getDeviceInfo() { return _deviceInfo; }

    /**
     * Get onvif media profiles
     */
    OnvifMediaProfileMap getMediaProfilesInfo() { return _mediaProfile; }

    /**
     * Select primary/secondary stream in media profile
     */
    std::vector<OnvifMediaProfile> selectStreamUrls(bool has_secondary = true);

    /**
     * If device has PTZ capability
     */
    bool enablePTZ() { return _ptzProfile.isAbsMoveEnable || _ptzProfile.isConsMoveEnable || _ptzProfile.isRelMoveEnable; }

    /**
     * Get onvif ptz profile
     */
    OnvifPTZProfile getPTZProfile() { return _ptzProfile; }

    /**
     * Execute PTZ Absolute Move 
     */
    bool PTZ_AbsoluteMove(float pan, float tilt, float zoom);

    /**
     * Execute PTZ Absolute Move with speed
     */
    bool PTZ_AbsoluteMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed);

    /**
     * Get current cordinates of len
     */
    tt__MoveStatus PTZ_GetStatus(float &pan, float &tilt, float &zoom);

    /**
     * Execute PTZ Continuous Move, timeout in second
     */
    bool PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout = 0);

    /**
     * Execute Stop PTZ Continuous Move
     */
    bool PTZ_Stop(bool panTilt, bool zoom);

    /**
     * Execute PTZ Relative Move 
     */
    bool PTZ_RelativeMove(float pan, float tilt, float zoom);

    /**
     * Execute PTZ Relative Move with speed
     */
    bool PTZ_RelativeMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed);

    std::string getSoapErrMsg() { return _soapErrMsg; }

private:
    bool destroyControl();

    void reportError();

    /**
     * set timestamp and authentication credentials in a request message
     */
    bool setCredentials();

    /**
     * get device information by soap protocol
     */
    bool getDeviceInformation();

    /**
     * get device capabilities by soap protocol
     */
    bool getDeviceCapabilities();

    /**
     * get network interface by soap protocol
     */
    bool getNetworkInterfaces();

    /**
     * get media profiles by soap protocol
     */
    bool getMediaProfiles();

private:
    // Device information
    OnvifDeviceInfo _deviceInfo;

    // Media profiles
    OnvifMediaProfileMap _mediaProfile;

    // PTZ configuration
    OnvifPTZProfile _ptzProfile;

    std::string _soapErrMsg;

private:
    soap   *_m_soap = nullptr;  //Soap for onvif
    DeviceBindingProxy  *_proxyDevice  = nullptr;    //Device API
    MediaBindingProxy   *_proxyMedia   = nullptr;    //Media API
    ImagingBindingProxy *_proxyImaging = nullptr;    //Imaging API
    PTZBindingProxy     *_proxyPTZ     = nullptr;    //PTZ API

    std::string _strDeviceIp;
    std::string _strUsername;
    std::string _strPassword;
};

} // namespace managerkit


#endif // S3PLUGINKIT_ONVIFCONTROL_H