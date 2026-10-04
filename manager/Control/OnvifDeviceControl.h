#ifndef S3PLUGINKIT_ONVIFCONTROL_H
#define S3PLUGINKIT_ONVIFCONTROL_H

#include <atomic>
#include <map>
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
#include "Control/CameraDeviceControl.h"

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


// The operations CameraController uses are virtual so unit tests can substitute a fake device
// (see CameraController::createDeviceControl).
class OnvifControl : public CameraDeviceControl {
public:
    using Ptr = std::shared_ptr<OnvifControl>;

    OnvifControl(std::string strDeviceIp, std::string strUsername = "", std::string strPassword = "");
    ~OnvifControl() override;

    bool connect() override;

    void disconnect() override;

    ControlProtocol protocol() const override { return ControlProtocol::ONVIF; }

    ConnectError lastConnectError() const override { return _lastConnectError.load(); }

    std::string lastError() override { return getSoapErrMsg(); }

    /**
     * Classify a failed GetDeviceInformation: `soap_error` is soap->error, `fault` the text of
     * soap_stream_fault. Only an answer that is clearly not ONVIF (HTTP 404/405, a body that is
     * not SOAP) is NOT_THIS_PROTOCOL; a timeout is always UNREACHABLE.
     * Static and public so it can be unit tested without a camera.
     */
    static ConnectError classifyConnectError(int soap_error, const std::string &fault);

    /**
     * Cheap health check for a controller that is already connected: one
     * authenticated GetDeviceInformation. False when the camera does not answer,
     * rejects the credentials, or reports a different device than the one
     * connect() read the capabilities from — all cases that need a full connect().
     */
    bool probeConnection() override;

    /**
     * Re-read the media profiles of a connected camera (GetProfiles plus one encoder
     * config read per profile). Lets the maintenance cycle notice a configuration
     * changed on the camera's own web page, and drive the config retry engine,
     * without redoing the whole connect() handshake.
     */
    bool refreshMediaProfiles() override;

    /**
     * Get onvif device info
     */
    OnvifDeviceInfo getDeviceInfo() override { return _deviceInfo; }

    /**
     * Get onvif media profiles
     */
    OnvifMediaProfileMap getMediaProfilesInfo() override { return _mediaProfile; }

    /**
     * Select primary/secondary stream in media profile
     */
    std::vector<OnvifMediaProfile> selectStreamUrls(bool has_secondary = true);

    /**
     * If device has PTZ capability
     */
    bool enablePTZ() override { return _ptzProfile.isAbsMoveEnable || _ptzProfile.isConsMoveEnable || _ptzProfile.isRelMoveEnable; }

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
    bool enableFocus() override { return _imageProfile.isFocusEnable; }

    /**
     * If device has Iris control capability
     */
    bool enableIris() override { return _imageProfile.isIrisEnable; }

    /**
     * If device has Relay Output capability
     */
    bool enableRelayOutput() override { return !_relayOutputProfiles.empty(); }

    /**
     * Get onvif ptz profile
     */
    OnvifPTZProfile getPTZProfile() override { return _ptzProfile; }

    /**
     * Check whether the SOAP session is still active (not disconnected)
     */
    bool isConnected() const override { return _m_soap != nullptr; }

    /**
     * Check whether a PTZ or relay control operation is in progress
     */
    bool isControlled() const override { return _isControlled.load() > 0; }

    /**
     * Get imaging (focus/iris) profile — capabilities discovered after connect()
     */
    OnvifImageProfile getImageProfile() const override { return _imageProfile; }

    /**
     * Get relay output profiles — populated after connect()
     */
    std::vector<OnvifRelayOutputProfile> getRelayOutputProfiles() const override { return _relayOutputProfiles; }

    /**
     * Get audio output profile — populated after connect()
     */
    OnvifAudioOutputProfile getAudioOutputProfile() const override { return _audioOutputProfile; }

    /**
     * Get audio input profile — populated after connect()
     */
    OnvifAudioInputProfile getAudioInputProfile() const override { return _audioInputProfile; }

    /**
     * Execute PTZ Absolute Move 
     */
    bool PTZ_AbsoluteMove(float pan, float tilt, float zoom) override;

    /**
     * Execute PTZ Absolute Move with speed
     */
    bool PTZ_AbsoluteMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) override;

    /**
     * Get current cordinates of len
     */
    PtzMoveStatus PTZ_GetStatus(float &pan, float &tilt, float &zoom) override;

    /**
     * Same, with the ONVIF move status
     */
    tt__MoveStatus PTZ_GetMoveStatus(float &pan, float &tilt, float &zoom);

    /**
     * Execute PTZ Continuous Move, timeout in second
     */
    bool PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout = 0) override;

    /**
     * Execute Stop PTZ Continuous Move
     */
    bool PTZ_Stop(bool panTilt, bool zoom) override;

    /**
     * Execute PTZ Relative Move 
     */
    virtual bool PTZ_RelativeMove(float pan, float tilt, float zoom);

    /**
     * Execute PTZ Relative Move with speed
     */
    bool PTZ_RelativeMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) override;

    /**
     * Execute PTZ Goto Home Position
     */
    bool PTZ_GotoHomePosition(float panSpeed, float tiltSpeed, float zoomSpeed) override;

    /**
     * Get last soap error message
     */
    virtual std::string getSoapErrMsg() { return _soapErrMsg; }

    /**
     * Execute PTZ Goto Preset
     */
    bool PTZ_GotoPreset(const std::string &presetToken, float panSpeed, float tiltSpeed, float zoomSpeed) override;

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
    bool setVideoEncoderConfigByToken(const std::string& token, const VideoEncoderConfig& vConfigNew) override;

    /**
    * @brief Get the video encoder configuration by profile token.
    *
    * @param token Profile token.
    * @param vConfig Output video encoder configuration.
    * @return true if the configuration is retrieved successfully; otherwise, false.
    */
    bool getVideoEncoderConfigByToken(const std::string& token, VideoEncoderConfig& vConfig) override;

    /**
    * Set the camera time to match the current system time. Virtual so the maintenance cycle can be
    * driven by a fake device in unit tests.
    */
    void setCameraTimeManual() override;

    /**
     * Move focus continuously. speed > 0 = focus in (near), speed < 0 = focus out (far).
     * Range [-1.0, 1.0]. Requires enableImaging() && _imageProfile.isFocusEnable.
     */
    bool Imaging_FocusMove(float speed) override;

    /**
     * Stop ongoing continuous focus move.
     */
    bool Imaging_FocusStop() override;

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
    bool PTZ_AuxiliaryCommand(const std::string &auxiliaryData) override;

    /**
     * Set the state of a relay output (on/off).
     * The relayToken must match one of the tokens returned by getRelayOutputTokens().
     */
    bool Relay_SetOutputState(const std::string &relayToken, bool active) override;

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
    SDCardInformation getSDCardInfo() override { return _sdCardInfo; }

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

    bool supportsAudioBackchannel() override { return supportAudioBackchannel(); }
    // setupAudioBackchannel(std::string &, ...) as RTSP_ONVIF.
    bool setupAudioBackchannel(AudioBackchannelInfo &out, bool sharedDspPipeline) override;

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
    std::atomic<ConnectError> _lastConnectError {ConnectError::NONE};
};

} // namespace managerkit


#endif // S3PLUGINKIT_ONVIFCONTROL_H