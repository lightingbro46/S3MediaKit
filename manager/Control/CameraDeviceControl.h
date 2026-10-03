#ifndef CONTROL_CAMERADEVICECONTROL_H
#define CONTROL_CAMERADEVICECONTROL_H

#include <cmath>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Common/DeviceControl.h"

namespace managerkit {

// Device model shared by every camera control protocol. The structs keep their historical Onvif*
// names: CameraStatistic, the REST API and the video config rules read them whatever protocol
// filled them in.

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
    int width = 0;
    int height = 0;
    int bitrate = 0;
    float fps = 0.0f;
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

struct OnvifAudioOutputProfile {
    bool enable = false;
    std::string token;
    std::string name;
    std::string outputToken;
    std::string profileToken;
    int outputLevel = 0;
    std::string sendPrimacy;
    std::string decoderToken;
    std::vector<std::string> supportedDecodingEncodings;

    void clear() {
        enable = false;
        token.clear();
        name.clear();
        outputToken.clear();
        profileToken.clear();
        sendPrimacy.clear();
        decoderToken.clear();
        supportedDecodingEncodings.clear();
        outputLevel = 0;
    }

    bool operator==(const OnvifAudioOutputProfile &o) const {
        return  enable == o.enable &&
                token == o.token &&
                name == o.name && 
                outputToken == o.outputToken && 
                profileToken == o.profileToken && 
                outputLevel == o.outputLevel &&
                sendPrimacy == o.sendPrimacy &&
                decoderToken == o.decoderToken &&
                supportedDecodingEncodings == o.supportedDecodingEncodings;
    }
    bool operator!=(const OnvifAudioOutputProfile &o) const { return !(*this == o); }
};

struct OnvifAudioInputProfile {
    bool enable = false;
    std::string token;
    std::string name;
    std::string sourceToken;
    std::string encoderToken;
    std::string acodec;
    int channelNo = 0; 
    std::string sampleRate;
    std::string sampleBit;

    void clear() {
        enable = false;
        token.clear();
        name.clear();
        sourceToken.clear();
        encoderToken.clear();
        acodec.clear();
        channelNo = 0;
        sampleRate.clear();
        sampleBit.clear();
    }

    bool operator==(const OnvifAudioInputProfile &o) const {
        return  enable == o.enable &&
                token == o.token &&
                name == o.name &&
                sourceToken == o.sourceToken &&
                encoderToken == o.encoderToken &&
                acodec == o.acodec &&
                channelNo == o.channelNo &&
                sampleRate == o.sampleRate &&
                sampleBit == o.sampleBit;
    }
    bool operator!=(const OnvifAudioInputProfile &o) const { return !(*this == o); }
};

struct OnvifImageProfile {
    // First video source token (from media profiles) used by the Imaging API
    std::string videoSourceToken;

    // Focus move capabilities (via ONVIF Imaging service, if supported by binding)
    bool isFocusAbsEnable = false;
    bool isFocusConsEnable = false;
    bool isFocusRelEnable = false;
    bool isFocusAutoSupported = false;
    bool isFocusEnable = false;

    // Iris continuous move capability (via ONVIF Imaging service, if supported by binding)
    bool isIrisAutoSupported = false;
    bool isIrisEnable = false;
    float irisMin = 0.0f;
    float irisMax = 1.0f;

    bool operator==(const OnvifImageProfile &o) const {
        return videoSourceToken == o.videoSourceToken &&
            isFocusAbsEnable == o.isFocusAbsEnable &&
            isFocusConsEnable == o.isFocusConsEnable &&
            isFocusRelEnable == o.isFocusRelEnable &&
            isFocusEnable == o.isFocusEnable &&
            isFocusAutoSupported == o.isFocusAutoSupported &&
            isIrisEnable == o.isIrisEnable &&
            isIrisAutoSupported == o.isIrisAutoSupported;
    }

    bool operator!=(const OnvifImageProfile &o) const { return !(*this == o); }
};

struct OnvifRelayOutputProfile {
    std::string token;
    std::string name;
    bool isActive = false;

    bool operator==(const OnvifRelayOutputProfile &o) const {
        return token == o.token && name == o.name && isActive == o.isActive;
    }

    bool operator!=(const OnvifRelayOutputProfile &o) const { return !(*this == o); }
};

struct RecordingInformation {
    int streamType = -1;
    std::string uri;
    std::string recordingToken;
    std::string trackToken;
    time_t earliestRecording = 0;
    time_t latestRecording = 0;
    VideoEncoderConfig vEncoder;
    bool hasRecord = false;
};

struct TrackStateEvent {
    std::string recordingToken;
    std::string trackToken;
    time_t time = 0;
    bool isDataPresent = false;
    bool isStartState = false;
};

struct VideoSegment {
    std::string recordingToken;
    std::string trackToken;
    time_t start_time = 0;
    time_t end_time = 0;
};

struct SDCardInformation {
    std::string type;
    uint64_t DataFrom = 0;
    uint64_t DataUntil = 0;
    int NumberRecordings = 0;

    bool isSDSupport() const {
        return DataFrom != 0 && DataUntil != 0 && NumberRecordings != 0;
    }
};

using OnvifMediaProfileMap = std::vector<OnvifMediaProfile>;

// Protocol a camera is controlled with. A camera is driven by at most one at a time.
enum class ControlProtocol { NONE, ONVIF, PSIA };

// Movement state reported by PTZ_GetStatus. UNKNOWN also means the status request failed.
enum class PtzMoveStatus { IDLE, MOVING, UNKNOWN };

// Why the last connect() failed; decides whether the AUTO mode may try the other protocol.
enum class ConnectError {
    NONE,
    NOT_THIS_PROTOCOL, // the device answered, but not in this protocol (HTTP 404/405, not SOAP/XML)
    AUTH_FAILED,       // the device speaks the protocol and rejected the credentials
    UNREACHABLE,       // no answer: refused, reset, timed out
    OTHER
};

// "" for NONE, "ONVIF", "PSIA".
const char *controlProtocolToString(ControlProtocol protocol);
// Case-insensitive; anything else is NONE.
ControlProtocol controlProtocolFromString(const std::string &str);
const char *connectErrorToString(ConnectError err);
// Normalizes a controlProtocol option: "ONVIF" or "PSIA" (any case), anything else is "AUTO".
std::string controlProtocolSetting(const std::string &value);
// Range "min ... max" in a few steps, as the encoder options report bitrates to the client.
std::string bitrateRangeToString(int minBitrate, int maxBitrate);

// What CameraController needs from a camera, whatever protocol it speaks. Every call is blocking
// and runs on the camera's device queue; the getters of cached state may be called from any thread.
class CameraDeviceControl : public DeviceControl {
public:
    using Ptr = std::shared_ptr<CameraDeviceControl>;

    virtual ControlProtocol protocol() const = 0;
    // Classification of the last failed connect(); NONE after a successful one.
    virtual ConnectError lastConnectError() const = 0;
    // Last device error, for logs and API error messages.
    virtual std::string lastError() = 0;

    virtual bool probeConnection() = 0;
    virtual bool isConnected() const = 0;
    virtual bool isControlled() const = 0;
    virtual void setCameraTimeManual() = 0;

    virtual OnvifDeviceInfo getDeviceInfo() = 0;
    virtual OnvifMediaProfileMap getMediaProfilesInfo() = 0;
    virtual bool refreshMediaProfiles() = 0;
    virtual bool getVideoEncoderConfigByToken(const std::string &token, VideoEncoderConfig &config) = 0;
    virtual bool setVideoEncoderConfigByToken(const std::string &token, const VideoEncoderConfig &config) = 0;

    virtual OnvifPTZProfile getPTZProfile() = 0;
    virtual bool enablePTZ() = 0;
    virtual bool PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout = 0) = 0;
    virtual bool PTZ_RelativeMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) = 0;
    virtual bool PTZ_AbsoluteMove(float pan, float tilt, float zoom) = 0;
    virtual bool PTZ_AbsoluteMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) = 0;
    virtual bool PTZ_Stop(bool panTilt, bool zoom) = 0;
    virtual bool PTZ_GotoPreset(const std::string &presetToken, float panSpeed, float tiltSpeed, float zoomSpeed) = 0;
    virtual bool PTZ_GotoHomePosition(float panSpeed, float tiltSpeed, float zoomSpeed) = 0;
    // ONVIF auxiliary data ("tt:Iris|open", "tt:Iris|close"); other protocols map what they can.
    virtual bool PTZ_AuxiliaryCommand(const std::string &auxiliaryData) = 0;
    virtual PtzMoveStatus PTZ_GetStatus(float &pan, float &tilt, float &zoom) = 0;

    virtual OnvifImageProfile getImageProfile() const = 0;
    virtual bool enableFocus() = 0;
    virtual bool enableIris() = 0;
    virtual bool Imaging_FocusMove(float speed) = 0;
    virtual bool Imaging_FocusStop() = 0;

    virtual std::vector<OnvifRelayOutputProfile> getRelayOutputProfiles() const = 0;
    virtual bool enableRelayOutput() = 0;
    virtual bool Relay_SetOutputState(const std::string &relayToken, bool active) = 0;

    // Only ONVIF has these; the defaults mean "not supported".
    virtual SDCardInformation getSDCardInfo() { return SDCardInformation(); }
    virtual OnvifAudioOutputProfile getAudioOutputProfile() const { return OnvifAudioOutputProfile(); }
    virtual OnvifAudioInputProfile getAudioInputProfile() const { return OnvifAudioInputProfile(); }
};

} // namespace managerkit

#endif // CONTROL_CAMERADEVICECONTROL_H
