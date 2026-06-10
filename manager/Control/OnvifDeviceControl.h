#ifndef S3PLUGINKIT_ONVIFCONTROL_H
#define S3PLUGINKIT_ONVIFCONTROL_H

#include <map>
#include "soapDeviceBindingProxy.h"
#include "soapMediaBindingProxy.h"
#include "soapMedia2BindingProxy.h"
#include "soapImagingBindingProxy.h"
#include "soapPTZBindingProxy.h"
#include "Common/DeviceControl.h"

namespace managerkit {

struct OnvifDeviceInfo {
    std::string manufacturer;
    std::string model;
    std::string firmwareVersion;
    std::string serialNumber;
    std::string hardwareId;
    std::string macAddress;

    bool operator==(const OnvifDeviceInfo &o) const {
        return manufacturer == o.manufacturer && model == o.model
            && firmwareVersion == o.firmwareVersion && serialNumber == o.serialNumber
            && hardwareId == o.hardwareId && macAddress == o.macAddress;
    }
    bool operator!=(const OnvifDeviceInfo &o) const { return !(*this == o); }
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

    bool isPresetEnable = false;
    struct PTZPreset {
        std::string Token;
        std::string Name;
        float absPan = -1;
        float absTilt = -1;
        float absZoom = -1;

        bool operator==(const PTZPreset &o) const {
            return Token == o.Token && Name == o.Name
                && absPan == o.absPan && absTilt == o.absTilt && absZoom == o.absZoom;
        }
    };
    using PTZPresetMap = std::unordered_map<std::string /*Token*/, PTZPreset>;
    PTZPresetMap presetMap; // pair of preset token and preset name
    bool isHomePresetEnable = false;
    std::string homePresetToken;

    bool operator==(const OnvifPTZProfile &o) const {
        return strMediaProfileToken == o.strMediaProfileToken
            && isAbsMoveEnable == o.isAbsMoveEnable
            && absMinPan == o.absMinPan && absMaxPan == o.absMaxPan
            && absMinTilt == o.absMinTilt && absMaxTilt == o.absMaxTilt
            && absMinZoom == o.absMinZoom && absMaxZoom == o.absMaxZoom
            && isConsMoveEnable == o.isConsMoveEnable
            && consMinPan == o.consMinPan && consMaxPan == o.consMaxPan
            && consMinTilt == o.consMinTilt && consMaxTilt == o.consMaxTilt
            && consMinZoom == o.consMinZoom && consMaxZoom == o.consMaxZoom
            && isRelMoveEnable == o.isRelMoveEnable
            && relMinPan == o.relMinPan && relMaxPan == o.relMaxPan
            && relMinTilt == o.relMinTilt && relMaxTilt == o.relMaxTilt
            && relMinZoom == o.relMinZoom && relMaxZoom == o.relMaxZoom
            && isPresetEnable == o.isPresetEnable
            && presetMap == o.presetMap
            && isHomePresetEnable == o.isHomePresetEnable
            && homePresetToken == o.homePresetToken;
    }
    bool operator!=(const OnvifPTZProfile &o) const { return !(*this == o); }
};

enum class VideoConfigSetState {NEW, PROCESSING, SUCCESS, FAILED, EXTERNAL};
static std::unordered_map<VideoConfigSetState, std::string> configStateToString = {
    {VideoConfigSetState::NEW, "NEW"},
    {VideoConfigSetState::PROCESSING, "PROCESSING"},
    {VideoConfigSetState::SUCCESS, "SUCCESS"},
    {VideoConfigSetState::FAILED, "FAILED"},
    {VideoConfigSetState::EXTERNAL, "EXTERNAL"}
};

struct VideoEncoderConfig {
    std::string vcodec;
    int width;
    int height;
    int bitrate;
    float fps;
    float quality;

    struct ConfigState {
        int retry_time = 5;
        std::string status = configStateToString[VideoConfigSetState::EXTERNAL];
    };
    ConfigState state;
    
    bool operator==(const VideoEncoderConfig& other) const {
        return vcodec   == other.vcodec &&
               width    == other.width &&
               height   == other.height &&
               bitrate  == other.bitrate &&
               isEqual(fps, other.fps);
    }
    
    bool operator!=(const VideoEncoderConfig& other) const {
        return !(*this == other);
    }

    using VideoEncoderConfigMap = std::unordered_map<std::string /*profile token*/, VideoEncoderConfig>;
private:
    static bool isEqual(float a, float b, float eps = 1e-6f) {
        return std::fabs(a - b) < eps;
    }
};

struct VideoEncoderConfigOption {
    std::vector<std::pair<int,int> > ResolutionsAvailable;
    std::string BitRateRange;
    std::pair<float,float> QualityRange;
    std::string FrameRatesSupported;
    bool FPSEditable = false;
    bool bitrateEditable = false;
    bool resolutionEditable = false;
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
    bool videoEncEditable = false;
    bool videoConfigEditable = false;
    bool hasAudio = false;
    std::string acodec;
    int channelNo;
    std::string sampleRate;
    std::string sampleBit;

    std::unordered_map<std::string, VideoEncoderConfigOption> vEncoderOptionMap;

    bool operator==(const OnvifMediaProfile &o) const {
        return token == o.token && url == o.url && 
            hasVideo == o.hasVideo && 
            vcodec == o.vcodec && 
            width == o.width && 
            height == o.height && 
            bitrate == o.bitrate && 
            fps == o.fps && 
            quality == o.quality && 
            videoEncEditable == o.videoEncEditable && 
            videoConfigEditable == o.videoConfigEditable && 
            hasAudio == o.hasAudio && 
            acodec == o.acodec && 
            channelNo == o.channelNo && 
            sampleRate == o.sampleRate && 
            sampleBit == o.sampleBit;
    }
    bool operator!=(const OnvifMediaProfile &o) const { return !(*this == o); }

    bool isVideoConfigEqual(const VideoEncoderConfig &o) const {
        return vcodec == o.vcodec && 
            width == o.width && 
            height == o.height && 
            bitrate == o.bitrate && 
            fps == o.fps;
    }
};

using OnvifMediaProfileMap = std::vector<OnvifMediaProfile>;

class OnvifControl : public DeviceControl {
public:
    using Ptr = std::shared_ptr<OnvifControl>;

    OnvifControl(std::string strDeviceIp, std::string strUsername = "", std::string strPassword = "");
    ~OnvifControl() override;

    bool connect() override;

    void disconnect() override;

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

    /**
     * Execute PTZ Goto Home Position
     */
    bool PTZ_GotoHomePosition(float panSpeed, float tiltSpeed, float zoomSpeed);

    /**
     * Get last soap error message
     */
    std::string getSoapErrMsg() { return _soapErrMsg; }

    /**
     * Execute PTZ Goto Preset
     */
    bool PTZ_GotoPreset(const std::string &presetToken, float panSpeed, float tiltSpeed, float zoomSpeed);

    /**
     * Execute PTZ Set Preset
     */
    bool PTZ_SetPreset(const std::string &presetName, const std::string &presetToken, float &pan, float &tilt, float &zoom);

    /**
    * @brief Send a request to set the video encoder configuration on the camera.
    *
    * @param token Profile token.
    * @param vConfigNew New video encoder configuration.
    * @return true if the request is sent successfully; otherwise, false.
    */
    bool setVideoEncoderConfigByToken(const std::string& token, const VideoEncoderConfig& vConfigNew);

    /**
    * @brief Get the video encoder configuration by profile token.
    *
    * @param token Profile token.
    * @param vConfig Output video encoder configuration.
    * @return true if the configuration is retrieved successfully; otherwise, false.
    */
    bool getVideoEncoderConfigByToken(const std::string& token, VideoEncoderConfig& vConfig);

    /**
    * Set the camera time to match the current system time.
    */
    void setCameraTimeManual();

private:
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

    /**
     * get preset
     */
    bool getPTZPresets();

    /**
     * get the camera's time by soap protocol
     */
    bool getCameraTime();

    /**
     * modify WS-Security (wsu:Created, wsu:Expires) before sending the request.
     */
    void installSoapHook(struct soap* soap, time_t offset);

    /**
    * Get tt__VideoEncoder2Configuration by profile token.
    */
    tt__VideoEncoder2Configuration* getConfigVideoEncoder2ByToken(const std::string& profileToken);

    /**
    * Save the profile configuration token and profile capabilities to the database.
    */
    void saveProfileConfigToDB(const std::string& camera_id, const OnvifMediaProfile& mProfile);

private:
    // Device information
    OnvifDeviceInfo _deviceInfo;

    // Media profiles
    OnvifMediaProfileMap _mediaProfile;

    // PTZ configuration
    OnvifPTZProfile _ptzProfile;

    std::string _soapErrMsg;

    struct CamTimeInfo {
        std::string TZ;
        time_t cam_time;
        time_t cam_sys_time;
    };
    CamTimeInfo _camTimeInfo;

private:
    soap   *_m_soap = nullptr;  //Soap for onvif
    DeviceBindingProxy  *_proxyDevice  = nullptr;    //Device API
    MediaBindingProxy   *_proxyMedia   = nullptr;    //Media API
    Media2BindingProxy   *_proxyMedia2   = nullptr;  //Media2 API
    ImagingBindingProxy *_proxyImaging = nullptr;    //Imaging API
    PTZBindingProxy     *_proxyPTZ     = nullptr;    //PTZ API

    std::string _strDeviceIp;
    std::string _strUsername;
    std::string _strPassword;
};

} // namespace managerkit


#endif // S3PLUGINKIT_ONVIFCONTROL_H