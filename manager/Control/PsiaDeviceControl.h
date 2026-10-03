#ifndef CONTROL_PSIADEVICECONTROL_H
#define CONTROL_PSIADEVICECONTROL_H

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "Control/CameraDeviceControl.h"

namespace PsiaCtrl {
// Timeout (milliseconds) of one PSIA HTTP request: connect, send and receive each.
extern const std::string kRequestTimeoutMs;
} // namespace PsiaCtrl

struct soap;
struct http_da_info;

namespace managerkit {

// Pure XML parsing / unit conversion behind PsiaControl. Exposed for the unit tests only.
namespace psia_detail {

// PSIA ResponseStatus: statusCode 1 = OK, 7 = Reboot Required; anything else is a failure.
struct ResponseStatus {
    int statusCode = 0;
    std::string statusString;
    std::string subStatusCode;
};
bool parseResponseStatus(const std::string &xml, ResponseStatus &out);
bool isResponseStatusOk(const ResponseStatus &status);

// DeviceInfo → OnvifDeviceInfo. False when the document is not a DeviceInfo.
bool parseDeviceInfo(const std::string &xml, OnvifDeviceInfo &out);

// "H.264" → "H264", "H.265"/"H.265+" → "H265", "MJPEG" → "JPEG"; the ONVIF names the rest of the
// server uses. Unknown names are returned unchanged.
std::string codecFromPsia(const std::string &psia);
// Inverse of codecFromPsia, for writing a channel back.
std::string codecToPsia(const std::string &vcodec);

// StreamingChannelList (or a single StreamingChannel) → one profile per channel that has video.
// `host` is the camera IP the RTSP URL is built for.
bool parseStreamingChannels(const std::string &xml, const std::string &host, OnvifMediaProfileMap &out);

// Encoder options from GET /PSIA/Streaming/channels/<id>/capabilities (opt=/min=/max= attributes).
void parseChannelCapabilities(const std::string &xml, OnvifMediaProfile &profile);

// Rewrites the video encoder nodes of a StreamingChannel document, leaving every other node
// (vendor extensions included) as the camera sent it. False when the document has no Video node.
bool applyVideoEncoderConfig(const std::string &channel_xml, const VideoEncoderConfig &config, std::string &out);

// Raw PTZ ranges of a channel, in the camera's own units.
struct PtzSpace {
    bool hasAbsolute = false;
    float absPanMin = 0, absPanMax = 3600;      // azimuth
    float absTiltMin = -900, absTiltMax = 2700; // elevation
    float absZoomMin = 10, absZoomMax = 10;     // absoluteZoom
    bool hasRelative = false;
    float relPanMax = 100, relTiltMax = 100, relZoomMax = 100;
    bool hasContinuous = false;
    float consPanMax = 100, consTiltMax = 100, consZoomMax = 100;
};
// PTZChanelCap → raw space + the normalized OnvifPTZProfile flags the controller reads
// (pan/tilt in [-1, 1], zoom in [0, 1] for absolute; [-1, 1] for relative/continuous).
bool parsePtzCapabilities(const std::string &xml, PtzSpace &space, OnvifPTZProfile &profile);

// Normalized [-1, 1] (or [0, 1] when `unit` is true) ↔ raw [min, max].
float toRaw(float normalized, float min, float max, bool unit = false);
float toNormalized(float raw, float min, float max, bool unit = false);

// PTZStatus → normalized position. False when there is no AbsoluteHigh.
bool parsePtzStatus(const std::string &xml, const PtzSpace &space, float &pan, float &tilt, float &zoom);

bool parsePresets(const std::string &xml, OnvifPTZProfile::PTZPresetMap &out);
bool parseOutputs(const std::string &xml, std::vector<OnvifRelayOutputProfile> &out);
// First PTZ channel id of a PTZChannelList, empty when there is none.
std::string parseFirstPtzChannel(const std::string &xml);

// Classifies a failed GET /PSIA/System/deviceInfo. `http_status` is 0 when no HTTP answer came
// back, `soap_error` is soap->error then; `is_device_info` tells whether a 200 carried a DeviceInfo.
ConnectError classifyConnectError(int http_status, int soap_error, bool is_device_info);

// Seconds east of UTC encoded in a POSIX TZ string as PSIA cameras report it: "CST-7:00:00" is UTC+7.
int posixTimeZoneOffset(const std::string &tz);
// Parses "YYYY-MM-DDTHH:MM:SS" with an optional "Z"/"+HH:MM" suffix into UTC seconds, using
// `default_offset` (seconds east of UTC) when there is no suffix. False on a malformed value.
bool parseLocalTime(const std::string &value, int default_offset, time_t &utc, bool &has_suffix);
// Rewrites a Time document to manual mode at `now` (UTC seconds) in the camera's own time zone.
bool applyManualTime(const std::string &time_xml, time_t now, std::string &out, time_t &camera_utc);

} // namespace psia_detail

// Camera control over PSIA (REST + XML under /PSIA, HTTP Digest with Basic fallback). Every call is
// blocking and runs on the camera's device queue like OnvifControl. The cached capabilities are read
// from _poller too, so they have their own short-held lock and never wait for a request.
class PsiaControl : public CameraDeviceControl {
public:
    using Ptr = std::shared_ptr<PsiaControl>;

    // `address` is "ip[:port]" of the camera web server, as for OnvifControl.
    PsiaControl(std::string address, std::string username = "", std::string password = "");
    ~PsiaControl() override;

    bool connect() override;
    void disconnect() override;

    ControlProtocol protocol() const override { return ControlProtocol::PSIA; }
    ConnectError lastConnectError() const override { return _lastConnectError.load(); }
    std::string lastError() override;

    bool probeConnection() override;
    bool isConnected() const override { return _connected.load(); }
    bool isControlled() const override { return _isControlled.load() > 0; }
    void setCameraTimeManual() override;

    OnvifDeviceInfo getDeviceInfo() override;
    OnvifMediaProfileMap getMediaProfilesInfo() override;
    bool refreshMediaProfiles() override;
    bool getVideoEncoderConfigByToken(const std::string &token, VideoEncoderConfig &config) override;
    bool setVideoEncoderConfigByToken(const std::string &token, const VideoEncoderConfig &config) override;

    OnvifPTZProfile getPTZProfile() override;
    bool enablePTZ() override;
    bool PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout = 0) override;
    bool PTZ_RelativeMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) override;
    bool PTZ_AbsoluteMove(float pan, float tilt, float zoom) override;
    bool PTZ_AbsoluteMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) override;
    bool PTZ_Stop(bool panTilt, bool zoom) override;
    bool PTZ_GotoPreset(const std::string &presetToken, float panSpeed, float tiltSpeed, float zoomSpeed) override;
    bool PTZ_GotoHomePosition(float panSpeed, float tiltSpeed, float zoomSpeed) override;
    // Only the iris commands ("tt:Iris|open", "tt:Iris|close") have a PSIA equivalent.
    bool PTZ_AuxiliaryCommand(const std::string &auxiliaryData) override;
    PtzMoveStatus PTZ_GetStatus(float &pan, float &tilt, float &zoom) override;

    OnvifImageProfile getImageProfile() const override;
    bool enableFocus() override;
    bool enableIris() override;
    bool Imaging_FocusMove(float speed) override;
    bool Imaging_FocusStop() override;

    std::vector<OnvifRelayOutputProfile> getRelayOutputProfiles() const override;
    bool enableRelayOutput() override;
    bool Relay_SetOutputState(const std::string &relayToken, bool active) override;

private:
    // One HTTP exchange. Retries once with Digest credentials after a 401 challenge. Returns the HTTP
    // status (0 when the request did not complete) and sets _lastError on failure.
    int request(const char *method, const std::string &path, const std::string &body, std::string &response);
    // request() + a 2xx status + (for writes) an OK ResponseStatus.
    bool get(const std::string &path, std::string &response);
    bool put(const std::string &path, const std::string &body);
    bool readDeviceInfo(OnvifDeviceInfo &info);
    bool readProfiles(OnvifMediaProfileMap &profiles);
    void readPtz();
    void readOutputs();
    bool sendPtz(const std::string &sub, const std::string &body);
    void setError(const std::string &err);

private:
    std::string _address;     // ip[:port]
    std::string _host;        // ip
    std::string _username;
    std::string _password;

    // Serialises every request on the soap context; recursive because public calls nest.
    std::recursive_mutex _soap_mtx;
    struct soap *_soap = nullptr;
    struct http_da_info *_digest = nullptr; // set once the camera sent a Digest challenge
    int _lastStatus = 0;      // HTTP status of the last request, 0 = no HTTP answer
    int _lastSoapError = 0;
    std::atomic<bool> _connected {false};

    // Guards everything below; held only to copy values in or out.
    mutable std::mutex _state_mtx;
    std::string _lastError;

    OnvifDeviceInfo _deviceInfo;
    OnvifMediaProfileMap _profiles;
    std::string _ptzChannel;  // empty: no PTZ
    psia_detail::PtzSpace _ptzSpace;
    OnvifPTZProfile _ptzProfile;
    OnvifImageProfile _imageProfile;
    std::string _videoInput = "1";
    std::vector<OnvifRelayOutputProfile> _outputs;

    std::atomic<int> _isControlled {0};
    std::atomic<ConnectError> _lastConnectError {ConnectError::NONE};
};

} // namespace managerkit

#endif // CONTROL_PSIADEVICECONTROL_H
