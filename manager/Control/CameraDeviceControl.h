#ifndef CONTROL_CAMERADEVICECONTROL_H
#define CONTROL_CAMERADEVICECONTROL_H

#include <cmath>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Common/DeviceControl.h"
#include "Backchannel/IBackchannelPusher.h"

namespace managerkit {

using mediakit::AudioBackchannelInfo;

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
enum class ControlProtocol { NONE, ONVIF, PSIA, ISAPI };

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

// "" for NONE, "ONVIF", "PSIA", "ISAPI".
const char *controlProtocolToString(ControlProtocol protocol);
// Case-insensitive; anything else is NONE.
ControlProtocol controlProtocolFromString(const std::string &str);
const char *connectErrorToString(ConnectError err);
// Normalizes a controlProtocol option: "ONVIF", "PSIA" or "ISAPI" (any case), anything else is "AUTO".
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

    // Audio to the device's speaker (ONVIF: RTSP backchannel, ISAPI: two-way audio). Blocking, on the
    // device queue, after a successful connect(). The default is "not supported".
    virtual bool supportsAudioBackchannel() { return false; }
    // Prepares the device and fills `out` with where BackchannelManager sends the audio. Credentials
    // are the caller's to add (the RTSP URI or the info fields).
    virtual bool setupAudioBackchannel(AudioBackchannelInfo &out, bool sharedDspPipeline) { return false; }
};

// ── Protocol selection shared by every controller (camera, speaker) ─────────────────────────────

// ONVIF → OnvifControl, PSIA → PsiaControl, ISAPI → IsapiControl, NONE → nullptr.
CameraDeviceControl::Ptr makeDeviceControl(ControlProtocol protocol, const std::string &address,
                                           const std::string &username, const std::string &password);

// "onvif,isapi,psia" → protocols in that order. Unknown names and repeats are skipped; an empty
// result falls back to ONVIF, ISAPI, PSIA.
std::vector<ControlProtocol> parseProtocolOrder(const std::string &order_str);

struct ProtocolProbeResult {
    CameraDeviceControl::Ptr winner; // connected controller, null when none answered
    std::string err_msg;             // why, when there is no winner
};
typedef std::function<CameraDeviceControl::Ptr(ControlProtocol)> DeviceControlFactory;
// Blocking, for the device queue: connects the protocols of `order` in turn and returns the first
// that connects. At most one controller is alive at a time: a loser is destroyed before the next is
// created. Moves on only when the device said "not this protocol", or when ONVIF rejected the
// credentials (a Hikvision camera with ONVIF off answers 401); the error reported then is ONVIF's.
ProtocolProbeResult probeControlProtocols(const std::vector<ControlProtocol> &order,
                                          const DeviceControlFactory &factory, const std::string &log_name);

// Which protocol a device is controlled with, and when AUTO probes again. Plain data, no thread or
// timer: the owner calls it on its own poller.
class ProtocolLock {
public:
    // The controlProtocol option: AUTO|ONVIF|PSIA|ISAPI, anything else is AUTO.
    void setSetting(const std::string &setting);
    // Protocol the device connected with before a restart: an AUTO device starts locked to it.
    void restore(ControlProtocol last);
    // Forgets the lock (address, credentials or protocol setting changed; or the device stopped
    // answering the locked protocol): AUTO probes on the next cycle.
    void unlock();

    bool isAuto() const { return _fixed == ControlProtocol::NONE; }
    // Protocol to create the controller with directly; NONE means probe.
    ControlProtocol fixedOrLocked() const;
    // Protocol of the last successful connect, NONE before any.
    ControlProtocol last() const { return _last; }

    // A controller connected with `protocol`. True when it differs from the previous protocol: the
    // owner drops the state that only made sense in the old one.
    bool onConnected(ControlProtocol protocol);
    // A connect() of the current controller failed. True when AUTO must unlock and probe again:
    // `reprobe_failures` NOT_THIS_PROTOCOL in a row (any other failure resets the count). A fixed
    // protocol never unlocks.
    bool onConnectError(ConnectError err, int reprobe_failures);

private:
    ControlProtocol _fixed = ControlProtocol::NONE;
    ControlProtocol _last = ControlProtocol::NONE;
    bool _locked = false;
    int _not_this_protocol_count = 0;
};

} // namespace managerkit

#endif // CONTROL_CAMERADEVICECONTROL_H
