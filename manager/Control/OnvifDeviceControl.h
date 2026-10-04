#ifndef S3PLUGINKIT_ONVIFCONTROL_H
#define S3PLUGINKIT_ONVIFCONTROL_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "soapDeviceBindingProxy.h"
#include "soapMediaBindingProxy.h"
#include "soapMedia2BindingProxy.h"
#include "soapImagingBindingProxy.h"
#include "soapPTZBindingProxy.h"
#include "soapSearchBindingProxy.h"
#include "soapReplayBindingProxy.h"
#include "soapRecordingBindingProxy.h"
#include "Common/DeviceControl.h"

namespace OnvifCtrl {
// TTL (seconds) of the cached camera-clock offset used to sign ONVIF requests.
// <= 0 disables the cache and refetches the camera time before every operation.
extern const std::string kClockOffsetTtlSec;
// How often (seconds) the maintenance cycle re-reads the media profiles of a camera
// that is already connected and has no pending config to push. <= 0 re-reads them
// on every cycle.
extern const std::string kProfileResyncIntervalSec;
// How often (seconds) the maintenance cycle re-checks the clock of a connected camera and pushes
// the server clock to it when they differ. <= 0 checks it on every cycle.
extern const std::string kClockSyncIntervalSec;
} // namespace OnvifCtrl

namespace managerkit {

struct OnvifHttpAuth;

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

// The operations CameraController uses are virtual so unit tests can substitute a fake device
// (see CameraController::createOnvifControl).
class OnvifControl : public DeviceControl {
public:
    using Ptr = std::shared_ptr<OnvifControl>;

    OnvifControl(std::string strDeviceIp, std::string strUsername = "", std::string strPassword = "");
    ~OnvifControl() override;

    bool connect() override;

    void disconnect() override;

    /**
     * Cheap health check for a controller that is already connected: one
     * authenticated GetDeviceInformation. False when the camera does not answer,
     * rejects the credentials, or reports a different device than the one
     * connect() read the capabilities from — all cases that need a full connect().
     */
    virtual bool probeConnection();

    /**
     * Re-read the media profiles of a connected camera (GetProfiles plus one encoder
     * config read per profile). Lets the maintenance cycle notice a configuration
     * changed on the camera's own web page, and drive the config retry engine,
     * without redoing the whole connect() handshake.
     */
    virtual bool refreshMediaProfiles();

    /**
     * Get onvif device info
     */
    OnvifDeviceInfo getDeviceInfo() { return _deviceInfo; }

    /**
     * Get onvif media profiles
     */
    virtual OnvifMediaProfileMap getMediaProfilesInfo() { return _mediaProfile; }

    /**
     * Select primary/secondary stream in media profile
     */
    std::vector<OnvifMediaProfile> selectStreamUrls(bool has_secondary = true);

    /**
     * If device has PTZ capability
     */
    virtual bool enablePTZ() { return _ptzProfile.isAbsMoveEnable || _ptzProfile.isConsMoveEnable || _ptzProfile.isRelMoveEnable; }

    /**
     * If device has Audio Output capability
     */
    bool enableAudioOutput() { return _audioOutputProfile.enable; }

    /**
     * If device has Audio Input (source) capability
     */
    bool enableAudioInput() { return _audioInputProfile.enable; }

    /**
     * If device has Focus capability
     */
    bool enableFocus() { return _imageProfile.isFocusEnable; }

    /**
     * If device has Iris control capability
     */
    bool enableIris() { return _imageProfile.isIrisEnable; }

    /**
     * If device has Relay Output capability
     */
    bool enableRelayOutput() { return !_relayOutputProfiles.empty(); }

    /**
     * Get onvif ptz profile
     */
    virtual OnvifPTZProfile getPTZProfile() { return _ptzProfile; }

    /**
     * Check whether the SOAP session is still active (not disconnected)
     */
    virtual bool isConnected() const { return _m_soap != nullptr; }

    /**
     * Check whether a PTZ or relay control operation is in progress
     */
    bool isControlled() const { return _isControlled.load() > 0; }

    /**
     * Get imaging (focus/iris) profile — capabilities discovered after connect()
     */
    OnvifImageProfile getImageProfile() const { return _imageProfile; }

    /**
     * Get relay output profiles — populated after connect()
     */
    std::vector<OnvifRelayOutputProfile> getRelayOutputProfiles() const { return _relayOutputProfiles; }

    /**
     * Get audio output profile — populated after connect()
     */
    OnvifAudioOutputProfile getAudioOutputProfile() const { return _audioOutputProfile; }

    /**
     * Get audio input profile — populated after connect()
     */
    OnvifAudioInputProfile getAudioInputProfile() const { return _audioInputProfile; }

    /**
     * Execute PTZ Absolute Move 
     */
    virtual bool PTZ_AbsoluteMove(float pan, float tilt, float zoom);

    /**
     * Execute PTZ Absolute Move with speed
     */
    virtual bool PTZ_AbsoluteMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed);

    /**
     * Get current cordinates of len
     */
    virtual tt__MoveStatus PTZ_GetStatus(float &pan, float &tilt, float &zoom);

    /**
     * Execute PTZ Continuous Move, timeout in second
     */
    virtual bool PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout = 0);

    /**
     * Execute Stop PTZ Continuous Move
     */
    virtual bool PTZ_Stop(bool panTilt, bool zoom);

    /**
     * Execute PTZ Relative Move 
     */
    virtual bool PTZ_RelativeMove(float pan, float tilt, float zoom);

    /**
     * Execute PTZ Relative Move with speed
     */
    virtual bool PTZ_RelativeMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed);

    /**
     * Execute PTZ Goto Home Position
     */
    virtual bool PTZ_GotoHomePosition(float panSpeed, float tiltSpeed, float zoomSpeed);

    /**
     * Get last soap error message
     */
    virtual std::string getSoapErrMsg() { return _soapErrMsg; }

    /**
     * Why the last failed request failed (network, credentials, other). Kept after disconnect(), so
     * a camera that cannot connect still says why. Readable from any thread.
     */
    DeviceFailure lastFailure() const { return static_cast<DeviceFailure>(_last_failure.load()); }

    /**
     * Classify a gSOAP failure: soap->error, the fault text, and whether the camera's 401 only
     * offered HTTP Basic that was refused.
     */
    static DeviceFailure classifyFailure(int soap_error, const std::string &fault, bool basic_refused);

    /**
     * Execute PTZ Goto Preset
     */
    virtual bool PTZ_GotoPreset(const std::string &presetToken, float panSpeed, float tiltSpeed, float zoomSpeed);

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
    virtual bool setVideoEncoderConfigByToken(const std::string& token, const VideoEncoderConfig& vConfigNew);

    /**
    * @brief Get the video encoder configuration by profile token.
    *
    * @param token Profile token.
    * @param vConfig Output video encoder configuration.
    * @return true if the configuration is retrieved successfully; otherwise, false.
    */
    bool getVideoEncoderConfigByToken(const std::string& token, VideoEncoderConfig& vConfig);

    /**
    * Set the camera time to match the current system time. Virtual so the maintenance cycle can be
    * driven by a fake device in unit tests.
    */
    virtual void setCameraTimeManual();

    /**
     * Move focus continuously. speed > 0 = focus in (near), speed < 0 = focus out (far).
     * Range [-1.0, 1.0]. Requires enableImaging() && _imageProfile.isFocusEnable.
     */
    bool Imaging_FocusMove(float speed);

    /**
     * Stop ongoing continuous focus move.
     */
    bool Imaging_FocusStop();

    /**
     * Move iris continuously. speed > 0 = iris open, speed < 0 = iris close.
     * Range [-1.0, 1.0]. Requires enableImaging() && _imageProfile.isIrisEnable.
     */
    bool Imaging_IrisMove(float /*speed*/);

    /**
     * Stop ongoing continuous iris move.
     */
    bool Imaging_IrisStop();

    /**
     * Send an ONVIF PTZ Auxiliary command (e.g. "tt:Iris|open", "tt:Iris|close").
     * The profileToken of the first profile in the PTZ profile is used.
     */
    bool PTZ_AuxiliaryCommand(const std::string &auxiliaryData);

    /**
     * Set the state of a relay output (on/off).
     * The relayToken must match one of the tokens returned by getRelayOutputTokens().
     */
    bool Relay_SetOutputState(const std::string &relayToken, bool active);

    /**
     * Find recorded video segments within the specified time range.
     * The returned segments are grouped by recording track and filtered
     * by the minimum segment duration.
    */
    std::unordered_map<int, std::vector<VideoSegment>> findVideoSegments(time_t startTime, time_t endTime, const int &min_segment_sec);

    /**
     * Get the recording information retrieved from the camera.
    */
    std::vector<RecordingInformation> getRecordingInformation() { return _recordingInformations; }

    /**
     * Get the SD card information retrieved from the camera.
    */
    SDCardInformation getSDCardInfo()  { return _sdCardInfo; }

    /**
     * Pick the media profile carrying the PTZ configuration, or null when the device reported
     * none. `Profiles` is an optional element and its entries may be null, so it must not be
     * indexed blindly.
     */
    static tt__Profile *pickPTZProfile(const _trt__GetProfilesResponse &resp);

    /**
     * True when a cached clock offset stamped at `cached_at` is still usable at
     * `now`. `ttl_sec` <= 0 disables the cache; a `now` before `cached_at` means
     * the local clock jumped back and the entry is discarded.
     * Static and public so it can be unit tested without a camera.
     */
    static bool clockOffsetFresh(time_t cached_at, time_t now, int ttl_sec);

    /**
     * True for the WS-Security faults that mean the camera rejected our
     * timestamp or digest, i.e. the ones that should drop the cached offset.
     * Static and public so it can be unit tested without a camera.
     */
    static bool isAuthOrTimestampFault(const std::string &fault);

    /**
     * Fill the PTZ ranges from a GetConfigurationOptions response, skipping every field the
     * device did not report. Pure parsing, no SOAP call: kept separate so it can be unit tested
     * against hand-built responses.
     *
     * Every space is optional in ONVIF, and a pan/tilt-only head (no zoom) is legal, so each
     * vector, each element and each XRange/YRange is checked on its own.
     */
    static void fillPTZSpaces(const _tptz__GetConfigurationOptionsResponse &resp, OnvifPTZProfile &out);

    /**
     * Returns whether audio backchannel is supported
     */
    bool supportAudioBackchannel() { return _audioOutputProfile.enable && !_audioOutputProfile.supportedDecodingEncodings.empty(); }

    /**
     * Configures the audio backchannel and retrieves its RTSP URI
     */
    bool setupAudioBackchannel(std::string &backchannelUri, const bool &sharedDspPipeline = false);

private:
    void reportError();

    /**
     * set timestamp and authentication credentials in a request message
     */
    bool setCredentials();

    /**
     * Run one authenticated gSOAP call on _m_soap, after setCredentials(). A camera whose ONVIF
     * service only accepts HTTP Digest ignores WS-Security and answers 401 with a challenge: the
     * challenge is learnt and the call sent once more, and every later call carries the Digest
     * header up front. Returns the gSOAP result of the last attempt.
     */
    int callWithAuth(const std::function<int()> &call);

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
     * query audio input/output capabilities via Media API
     */
    bool getAudioCapabilities();

    /**
     * Probe imaging move options; sets _ptzProfile.isFocusEnable and _videoSourceToken.
     */
    bool getImagingCapabilities();

    /**
    * get relay output by soap protocol
    */
    bool getRelayOutputCapabilities();

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
     * Stop an installed hook from rewriting timestamps. soap->fsend cannot be
     * restored safely once replaced, so the hook is switched off in place.
     */
    void disableSoapHook(struct soap* soap);

    /**
     * Store the camera-clock offset derived from the last successful
     * getCameraTime(), and stamp it with the current time.
     */
    void cacheClockOffset();

    /**
     * Drop the cached offset; the next setCredentials() refetches the camera time.
     */
    void invalidateClockOffset();

    /**
     * Camera-clock offset to sign with, refetching it only when the cache is
     * empty or older than `onvif.clockOffsetTtlSec`. Returns false when no
     * offset is available, in which case the local clock must be used.
     */
    bool clockOffset(time_t &offset);

    /**
    * Get tt__VideoEncoder2Configuration by profile token.
    */
    tt__VideoEncoder2Configuration* getConfigVideoEncoder2ByToken(const std::string& profileToken);

    /**
    * Save the profile configuration token and profile capabilities to the database.
    */
    void saveProfileConfigToDB(const std::string& camera_id, const OnvifMediaProfile& mProfile);

    std::string getReplayUri(const std::string& recordingToken);

    void getRecordingSearchResults();

    bool findEvents(time_t& startTime, time_t& endTime, const RecordingInformation &info, std::string& outSearchToken);

    bool getEventSearchResults(const std::string& searchToken, const RecordingInformation &info, std::vector<TrackStateEvent>& outEvents);

    void reconstructVideoSegments(const std::vector<TrackStateEvent>& events, const time_t& startTime, const time_t& endTime, const int &min_segment_sec, std::vector<VideoSegment>& outSegments);

    void endSearch(const std::string& searchToken);

    VideoEncoderConfig GetRecordingVideoEncoder(const std::string& recordingToken, const time_t& time);

    void getRecordingSummary();

    void getStorageConfiguration();
private:
    // Device information
    OnvifDeviceInfo _deviceInfo;

    // Media profiles
    OnvifMediaProfileMap _mediaProfile;

    // PTZ configuration
    OnvifPTZProfile _ptzProfile;

    // Audio output configuration
    OnvifAudioOutputProfile _audioOutputProfile;

    // Audio input configuration
    OnvifAudioInputProfile _audioInputProfile;

    // Imaging configuration
    OnvifImageProfile _imageProfile;

    // Relay output configuration
    std::vector<OnvifRelayOutputProfile> _relayOutputProfiles;

    std::string _soapErrMsg;
    std::atomic<int> _last_failure {static_cast<int>(DeviceFailure::None)};

    struct CamTimeInfo {
        std::string TZ;
        // Without these initializers both fields hold garbage until the first
        // successful getCameraTime(), and setCredentials() would sign with it.
        time_t cam_time = 0;
        time_t cam_sys_time = 0;
    };
    CamTimeInfo _camTimeInfo;

    // Cached camera-clock offset (camera UTC - local time), so that a full
    // GetSystemDateAndTime round trip is not needed before every operation.
    // Atomic because setCredentials() is also reached from findVideoSegments()
    // without _soap_mtx held, and the same OnvifControl is shared between the
    // camera's device queue and the MediaGapFillManager worker.
    std::atomic<time_t> _clock_offset {0};     // meaningless while _clock_offset_at == 0
    std::atomic<time_t> _clock_offset_at {0};  // 0 = no valid cache entry

    std::vector<RecordingInformation> _recordingInformations;

    SDCardInformation _sdCardInfo;
private:
    soap   *_m_soap = nullptr;  //Soap for onvif
    // HTTP authentication learnt from _m_soap's 401 challenges; lives and dies with _m_soap.
    std::unique_ptr<OnvifHttpAuth> _http_auth;
    DeviceBindingProxy  *_proxyDevice  = nullptr;    //Device API
    MediaBindingProxy   *_proxyMedia   = nullptr;    //Media API
    Media2BindingProxy   *_proxyMedia2   = nullptr;  //Media2 API
    ImagingBindingProxy *_proxyImaging = nullptr;    //Imaging API
    PTZBindingProxy     *_proxyPTZ     = nullptr;    //PTZ API
    SearchBindingProxy   *_proxySearch   = nullptr;  //Search API
    ReplayBindingProxy   *_proxyReplay   = nullptr;  //Replay API
    RecordingBindingProxy   *_proxyRecording   = nullptr;  //Recording API

    std::string _strDeviceIp;
    std::string _strUsername;
    std::string _strPassword;

    // Cached endpoint URLs — member strings replace thread_local to avoid
    // a dangling-pointer when soap_endpoint is accessed from a different
    // thread than the one that ran connect().
    std::string _strDeviceUrl;
    std::string _strMediaUrl;
    std::string _strImagingUrl;
    std::string _strPTZUrl;

    // Serialises all gSOAP operations on this instance.  recursive_mutex is
    // required because public methods call other public methods
    // (connect→disconnect, PTZ_SetPreset→PTZ_GetStatus, etc.).
    mutable std::recursive_mutex _soap_mtx;
    std::atomic<int> _isControlled {0}; // true if a PTZ or relay control operation is in progress
};

} // namespace managerkit


#endif // S3PLUGINKIT_ONVIFCONTROL_H