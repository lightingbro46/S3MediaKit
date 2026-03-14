#ifndef COMMON_CONFIG_H
#define COMMON_CONFIG_H

#include "Util/NoticeCenter.h"
#include "Util/mini.h"
#include "Util/onceToken.h"
#include "macros.h"
#include <functional>

namespace mediakit {

class ProtocolOption;

// Load the configuration file. If the configuration file does not exist, the default configuration will be exported and the configuration file will be generated.
// After the configuration file is loaded successfully, the kBroadcastUpdateConfig broadcast will be triggered.
// If the specified file name (ini_path) is empty, the default configuration file will be loaded.
// The default configuration file name is /path/to/your/exe.ini
// Returns true if the configuration file is loaded successfully, otherwise returns false.
bool loadIniConfig(const char *ini_path = nullptr);

// //////////Broadcast Name///////////
namespace Broadcast {

// Register or unregister MediaSource event broadcast
extern const std::string kBroadcastMediaChanged;
#define BroadcastMediaChangedArgs const bool &bRegist, MediaSource &sender

// Broadcast after recording mp4 file successfully
extern const std::string kBroadcastRecordMP4;
#define BroadcastRecordMP4Args const RecordInfo &info

// Broadcast after recording ts file
extern const std::string kBroadcastRecordTs;
#define BroadcastRecordTsArgs const RecordInfo &info

// Broadcast for receiving http api request
extern const std::string kBroadcastHttpRequest;
#define BroadcastHttpRequestArgs const Parser &parser, const HttpSession::HttpResponseInvoker &invoker, bool &consumed, SockInfo &sender

// In the http file server, broadcast for receiving http access to files or directories. Control access permissions to the http directory through this event.
extern const std::string kBroadcastHttpAccess;
#define BroadcastHttpAccessArgs const Parser &parser, const std::string &path, const bool &is_dir, const HttpSession::HttpAccessPathInvoker &invoker, SockInfo &sender

// In the http file server, broadcast before receiving http access to files or directories. Control the mapping from http url to file path through this event.
// By overriding the path parameter in this event, you can achieve the purpose of selecting different http root directories based on virtual hosts or apps.
extern const std::string kBroadcastHttpBeforeAccess;
#define BroadcastHttpBeforeAccessArgs const Parser &parser, std::string &path, SockInfo &sender

// Does this stream need authentication? If yes, call invoker and pass in realm, otherwise pass in an empty realm. If this event is not listened to, no authentication will be performed.
extern const std::string kBroadcastOnGetRtspRealm;
#define BroadcastOnGetRtspRealmArgs const MediaInfo &args, const RtspSession::onGetRealm &invoker, SockInfo &sender

// Request authentication user password event, user_name is the username, must_no_encrypt if true, then the plaintext password must be provided (because it is base64 authentication method at this time), otherwise it will lead to authentication failure.
// After getting the password, please call invoker and input the corresponding type of password and password type. The invoker will match the password when executing.
extern const std::string kBroadcastOnRtspAuth;
#define BroadcastOnRtspAuthArgs const MediaInfo &args, const std::string &realm, const std::string &user_name, const bool &must_no_encrypt, const RtspSession::onAuth &invoker, SockInfo &sender

// Push stream authentication result callback object
// If err is empty, it means authentication is successful.
using PublishAuthInvoker = std::function<void(const std::string &err, const ProtocolOption &option)>;

// Broadcast for receiving rtsp/rtmp push stream event. Control push stream authentication through this event.
extern const std::string kBroadcastMediaPublish;
#define BroadcastMediaPublishArgs const MediaOriginType &type, const MediaInfo &args, const Broadcast::PublishAuthInvoker &invoker, SockInfo &sender

// Playback authentication result callback object
// If err is empty, it means authentication is successful.
using AuthInvoker = std::function<void(const std::string &err)>;

// Broadcast for playing rtsp/rtmp/http-flv events. Control playback authentication through this event.
extern const std::string kBroadcastMediaPlayed;
#define BroadcastMediaPlayedArgs const MediaInfo &args, const Broadcast::AuthInvoker &invoker, SockInfo &sender

// Shell login authentication
extern const std::string kBroadcastShellLogin;
#define BroadcastShellLoginArgs const std::string &user_name, const std::string &passwd, const Broadcast::AuthInvoker &invoker, SockInfo &sender

// Broadcast for traffic reporting event after stopping rtsp/rtmp/http-flv session
extern const std::string kBroadcastFlowReport;
#define BroadcastFlowReportArgs const MediaInfo &args, const uint64_t &totalBytes, const uint64_t &totalDuration, const bool &isPlayer, SockInfo &sender

// This event will be broadcast after the stream is not found. Please pull the stream or other methods to generate the stream after listening to this event, so that you can pull the stream on demand.
extern const std::string kBroadcastNotFoundStream;
#define BroadcastNotFoundStreamArgs const MediaInfo &args, SockInfo &sender, const std::function<void()> &closePlayer

// Triggered when a stream is not consumed by anyone. The purpose is to achieve business logic such as actively disconnecting the pull stream when no one is watching.
extern const std::string kBroadcastStreamNoneReader;
#define BroadcastStreamNoneReaderArgs MediaSource &sender

// Triggered when rtp push stream is passively stopped.
extern const std::string kBroadcastSendRtpStopped;
#define BroadcastSendRtpStoppedArgs MultiMediaSourceMuxer &sender, const std::string &ssrc, const SockException &ex

// Update configuration file event broadcast. This broadcast will be triggered after the loadIniConfig function loads the configuration file successfully.
extern const std::string kBroadcastReloadConfig;
#define BroadcastReloadConfigArgs void

// Rtp server timeout
extern const std::string kBroadcastRtpServerTimeout;
#define BroadcastRtpServerTimeoutArgs uint16_t &local_port, const MediaTuple &tuple, int &tcp_mode, bool &re_use_port, uint32_t &ssrc

// Rtc transport sctp connection status
extern const std::string kBroadcastRtcSctpConnecting;
extern const std::string kBroadcastRtcSctpConnected;
extern const std::string kBroadcastRtcSctpFailed;
extern const std::string kBroadcastRtcSctpClosed;
#define BroadcastRtcSctpConnectArgs WebRtcTransport& sender

// rtc transport sctp send data
extern const std::string kBroadcastRtcSctpSend;
#define BroadcastRtcSctpSendArgs WebRtcTransport& sender, const uint8_t *&data, size_t& len

// rtc transport sctp receive data
extern const std::string kBroadcastRtcSctpReceived;
#define BroadcastRtcSctpReceivedArgs WebRtcTransport& sender, uint16_t &streamId, uint32_t &ppid, const uint8_t *&msg, size_t &len

// broadcast viewer count changes
extern const std::string kBroadcastPlayerCountChanged;
#define BroadcastPlayerCountChangedArgs const MediaTuple& args, const int& count

using SeekInvoker = std::function <void(const int64_t &)>;
// Broadcast for seeking rtsp/rtmp/http-flv events. Control playback seeking through this event.
extern const std::string kBroadcastMediaSeeked;
#define BroadcastMediaSeekedArgs const MediaTuple &args, const uint64_t &stamp, const Broadcast::SeekInvoker &invoker, SockInfo &sender

using Seek2Invoker = std::function <void(const uint64_t&, const std::map<uint64_t, std::string> &)>;
// Broadcast for seeking rtsp/rtmp/http-flv events. Control playback seeking through this event.
extern const std::string kBroadcastMediaSeeked2;
#define BroadcastMediaSeeked2Args const MediaTuple& args, const uint64_t &stamp, const uint64_t &max_duration, const Broadcast::Seek2Invoker &invoker

// Broadcast for restart server events. Control server restarting through this event.
extern const std::string kBroadcastRestartServer;
#define BroadcastRestartServerArgs void

// Broadcast for system alert events. Control server emit system alert through this event.
extern const std::string kBroadcastSystemAlert;
#define BroadcastSystemAlertArgs uint8_t &type, float &usage, float &threshold, bool &is_critical

// Broadcast for stream reader alert events. Control server emit stream reader alert through this event.
extern const std::string kBroadcastStreamReaderAlert;
#define BroadcastStreamReaderAlertArgs const std::string &camera_id, int &usage, int &threshold, bool &is_critical

// Register or unregister DeviceSource event broadcast
extern const std::string kBroadcastDeviceChanged;
#define BroadcastDeviceChangedArgs const bool &bRegist, DeviceSource &sender

// Broadcast for accessing device data. Control device authentication through this event.
extern const std::string kBroadcastDeviceAccess;
#define BroadcastDeviceAccessArgs const std::string &device_id, const std::string &jwt_token, const Broadcast::AuthInvoker &invoker

// Broadcast for device capabilities changed event.
extern const std::string kBroadcastDeviceCapsChanged;
#define BroadcastDeviceCapsChangedArgs const DeviceCapabilities &caps, DeviceSource &sender

// Broadcast for reloading API configuration.
extern const std::string kBroadcastReloadApiConfig;
#define BroadcastReloadApiConfigArgs void

using HealthInvoker = std::function<void(const std::string&, const int&)>;
// Healthcheck service event broadcast. Control healthcheck service through this event.
extern const std::string kBroadcastHealthCheckService;
#define BroadcastHealthCheckServiceArgs const std::vector<std::string> &origin_urls, const Broadcast::HealthInvoker &invoker

extern const std::string kBroadcastRecordMotion;
#define BroadcastRecordMotionArgs const MediaTuple &args, const bool &bActive

#define ReloadConfigTag ((void *)(0xFF))
#define RELOAD_KEY(arg, key)                                                                                           \
    do {                                                                                                               \
        decltype(arg) arg##_tmp = ::toolkit::mINI::Instance()[key];                                                    \
        if (arg == arg##_tmp) {                                                                                        \
            return;                                                                                                    \
        }                                                                                                              \
        arg = arg##_tmp;                                                                                               \
        InfoL << "reload config:" << key << "=" << arg;                                                                \
    } while (0)

// listen for configuration changes
#define LISTEN_RELOAD_KEY(arg, key, ...)                                                                               \
    do {                                                                                                               \
        static ::toolkit::onceToken s_token_listen([]() {                                                              \
            ::toolkit::NoticeCenter::Instance().addListener(                                                           \
                ReloadConfigTag, Broadcast::kBroadcastReloadConfig, [](BroadcastReloadConfigArgs) { __VA_ARGS__; });   \
        });                                                                                                            \
    } while (0)

#define GET_CONFIG(type, arg, key)                                                                                     \
    static type arg = ::toolkit::mINI::Instance()[key];                                                                \
    LISTEN_RELOAD_KEY(arg, key, { RELOAD_KEY(arg, key); });

#define GET_CONFIG_FUNC(type, arg, key, ...)                                                                           \
    static type arg;                                                                                                   \
    do {                                                                                                               \
        static ::toolkit::onceToken s_token_set([]() {                                                                 \
            static auto lam = __VA_ARGS__;                                                                             \
            static auto arg##_str = ::toolkit::mINI::Instance()[key];                                                  \
            arg = lam(arg##_str);                                                                                      \
            LISTEN_RELOAD_KEY(arg, key, {                                                                              \
                RELOAD_KEY(arg##_str, key);                                                                            \
                arg = lam(arg##_str);                                                                                  \
            });                                                                                                        \
        });                                                                                                            \
    } while (0)

} // namespace Broadcast

// //////////General Configuration///////////
namespace General {
// ID (GUID) of each media server
extern const std::string kMediaServerId;
// Traffic reporting event traffic threshold, unit KB, default 1MB
extern const std::string kFlowThreshold;
// Trigger kBroadcastStreamNoneReader event only after the stream has been unwatched for a certain period of time
// Default to trigger kBroadcastStreamNoneReader event after 5 seconds of no viewers
extern const std::string kStreamNoneReaderDelayMS;
// Stream registration timeout, after receiving the player's request, if the related stream is not found, the server will wait for a certain period of time,
// If the related stream is registered within this time, the server will immediately respond to the player that the playback is successful,
// Otherwise, it will wait for a maximum of kMaxStreamWaitTimeMS milliseconds and then respond to the player that the playback failed
extern const std::string kMaxStreamWaitTimeMS;
// Whether to enable virtual host
extern const std::string kEnableVhost;
// When pulling stream proxy, whether to delete the previous media stream data if the stream is disconnected and reconnected successfully, if deleted, it will start again,
// If not deleted, it will continue to write from the previous data (when recording hls/mp4, it will continue to write after the previous file)
extern const std::string kResetWhenRePlay;
// Merge write cache size (unit milliseconds), merge write refers to the server caching a certain amount of data before writing to the socket at once, which can improve performance but increase latency
// When enabled, TCP_NODELAY will be closed and MSG_MORE will be enabled at the same time
extern const std::string kMergeWriteMS;
// In the docker environment, the existence of the NVIDIA driver cannot be used to determine whether hardware transcoding is supported
extern const std::string kCheckNvidiaDev;
// Whether to enable ffmpeg log
extern const std::string kEnableFFmpegLog;
// Maximum wait time for uninitialized Track is 10 seconds, after timeout, uninitialized Track will be ignored
extern const std::string kWaitTrackReadyMS;
//Wait for the audio track to receive data for up to milliseconds, timeout and no audio data received at all, ignore audio track
//Speeding up some streaming metadata with packages means that there is audio, but there is actually no streaming time (such as GB28181 PS from many manufacturers)
extern const std::string kWaitAudioTrackDataMS;
// If the live stream has only one Track, wait for a maximum of 3 seconds, if no data from other Tracks is received after timeout, it is considered a single Track
// If the protocol metadata declares a specific number of tracks, there is no such waiting time
extern const std::string kWaitAddTrackMS;
// If the track is not ready, we will cache the frame data first, but there is a maximum number limit (100 frames is about 4 seconds) to prevent memory overflow
extern const std::string kUnreadyFrameCache;
// Whether to enable viewer count change event broadcast, set to 1 to enable, set to 0 to disable
extern const std::string kBroadcastPlayerCountChanged;
// Bound local network card ip
extern const std::string kListenIP;
} // namespace General

namespace Protocol {
static constexpr char kFieldName[] = "protocol.";
// Timestamp repair flag for this stream
extern const std::string kModifyStamp;
// Whether to enable audio for protocol conversion
extern const std::string kEnableAudio;
// Add silent audio, this switch is invalid when audio is closed
extern const std::string kAddMuteAudio;
// When there are no viewers, whether to close directly (instead of returning close through the on_none_reader hook)
// When this configuration is set to 1, if this stream has no viewers, it will not trigger the on_none_reader hook callback,
// Instead, it will directly close the stream
extern const std::string kAutoClose;
// When the continuous delay is interrupted, the unit is milliseconds, and the configuration file is used by default
extern const std::string kContinuePushMS;
// Smooth sending timer interval, unit is milliseconds, set to 0 to close; enabling it will affect CPU performance and increase memory
// Enabling this configuration can solve some problems where the stream is not sent smoothly, resulting in S3MediaKit forwarding not being smooth
extern const std::string kPacedSenderMS;

// Whether to enable conversion to HLS (MPEGTS)
extern const std::string kEnableHls;
// Whether to enable conversion to HLS (FMP4)
extern const std::string kEnableHlsFmp4;
// Whether to enable MP4 recording
extern const std::string kEnableMP4;
// Whether to enable conversion to RTSP/WebRTC
extern const std::string kEnableRtsp;
// Whether to enable conversion to RTMP/FLV
extern const std::string kEnableRtmp;
// Whether to enable conversion to HTTP-TS/WS-TS
extern const std::string kEnableTS;
// Whether to enable conversion to HTTP-FMP4/WS-FMP4
extern const std::string kEnableFMP4;

// Whether to treat MP4 recording as a viewer
extern const std::string kMP4AsPlayer;
// MP4 fragment size, unit is seconds
extern const std::string kMP4MaxSecond;
// MP4 recording save path
extern const std::string kMP4SavePath;

// HLS recording save path
extern const std::string kHlsSavePath;

// On-demand protocol conversion switch
extern const std::string kHlsDemand;
extern const std::string kRtspDemand;
extern const std::string kRtmpDemand;
extern const std::string kTSDemand;
extern const std::string kFMP4Demand;
// Application name for viewing live stream
extern const std::string kAppName;

// Whether to enable motion detection
extern const std::string kEnableMotion;
// Motion detection on-demand switch
extern const std::string kMotionDemand;
// GOP cache size, unit is frames
extern const std::string kGopCacheSize;
// ROI mask for motion detection
extern const std::string kRoiMask;
// Record stream in motion detection mode, only record when motion is detected
extern const std::string kRecordMotion;
// Record before motion is detected, unit is milliseconds, default is 0, which means no recording before motion is detected
extern const std::string kPreRecordMS;
// Record after motion is detected, unit is milliseconds, default is 15000, which means to continue recording for 15 seconds after motion is detected
extern const std::string kPostRecordMS;
} // !Protocol

// //////////HTTP configuration///////////
namespace Http {
// HTTP file sending cache size
extern const std::string kSendBufSize;
// HTTP maximum request byte size
extern const std::string kMaxReqSize;
// HTTP keep-alive seconds
extern const std::string kKeepAliveSecond;
// HTTP character encoding
extern const std::string kCharSet;
// HTTP server root directory
extern const std::string kRootPath;
// HTTP server virtual directory. Virtual directory name and file path are separated by ",", and multiple configuration paths are separated by ";", for example, path_d,d:/record;path_e,e:/record
extern const std::string kVirtualPath;
// HTTP 404 error prompt content
extern const std::string kNotFound;
// Whether to display the folder menu
extern const std::string kDirMenu;
// Forbidden cache file suffixes
extern const std::string kForbidCacheSuffix;
// You can put the real client IP address before the HTTP proxy in the HTTP header: https://github.com/S3MediaKit/S3MediaKit/issues/1388
extern const std::string kForwardedIpHeader;
// Whether to allow all cross-domain requests
extern const std::string kAllowCrossDomains;
// Whitelist of IP address ranges allowed to access HTTP API and HTTP file index. No restrictions are imposed when empty
extern const std::string kAllowIPRange;
} // namespace Http

// //////////SHELL configuration///////////
namespace Shell {
extern const std::string kMaxReqSize;
} // namespace Shell

// //////////RTSP Server Configuration///////////
namespace Rtsp {
// Is base64 authentication prioritized? Default is Md5 authentication
extern const std::string kAuthBasic;
// Handshake timeout, default 15 seconds
extern const std::string kHandshakeSecond;
// Keep-alive timeout, default 15 seconds
extern const std::string kKeepAliveSecond;

// Whether RTSP pull stream proxy is direct proxy
// Direct proxy supports any encoding format, but it will cause GOP cache unable to locate I-frame, which may lead to screen flickering
// And if it is TCP pull stream, if RTP is larger than MTU, it will not be able to use UDP proxy
// Assuming your pull stream source address is not 264 or 265 or AAC, then you can use direct proxy to support RTSP proxy
// Default to enable RTSP direct proxy, RTMP does not have these problems, it is forced to enable direct proxy
extern const std::string kDirectProxy;

// Whether RTSP forwarding uses low latency mode, when enabled, it will not cache RTP packets to improve concurrency and reduce one frame delay
extern const std::string kLowLatency;

// Force negotiation of RTP transport method (0: TCP, 1: UDP, 2: MULTICAST, -1: no restriction)
// When the client initiates RTSP SETUP, if the transport type is inconsistent with this configuration, it will return 461 Unsupport Transport
// Force the client to re-SETUP and switch to the corresponding protocol. Currently supports FFMPEG and VLC
extern const std::string kRtpTransportType;
} // namespace Rtsp

// //////////RTMP Server Configuration///////////
namespace Rtmp {
// Handshake timeout, default 15 seconds
extern const std::string kHandshakeSecond;
// Keep-alive timeout, default 15 seconds
extern const std::string kKeepAliveSecond;
// Whether direct proxy
extern const std::string kDirectProxy;
// Whether h265-rtmp uses enhanced (or domestic extension)
extern const std::string kEnhanced;
} // namespace Rtmp

// //////////RTP Configuration///////////
namespace Rtp {
// Maximum RTP packet MTU, smaller in public network
extern const std::string kVideoMtuSize;
// Maximum RTP packet MTU, smaller in public network
extern const std::string kAudioMtuSize;
// Maximum RTP packet length limit, unit KB
extern const std::string kRtpMaxSize;
// When RTP is packaged, low latency switch, default off (0), H264 has multiple slices (NAL) in one frame, in this case, if enabled, it may cause screen flickering
extern const std::string kLowLatency;
// Whether H264 RTP packaging mode uses stap-a mode (for compatibility with webrtc on older browsers) or Single NAL unit packet per H.264 mode
extern const std::string kH264StapA;
} // namespace Rtp

// //////////Multicast Configuration///////////
namespace MultiCast {
// Multicast allocation start address
extern const std::string kAddrMin;
// Multicast allocation end address
extern const std::string kAddrMax;
// Multicast TTL
extern const std::string kUdpTTL;
} // namespace MultiCast

// //////////Recording Configuration///////////
namespace Record {
// Application name for viewing recordings
extern const std::string kAppName;
// Duration of each MP4 file streaming, in milliseconds
extern const std::string kSampleMS;
// MP4 file write cache size
extern const std::string kFileBufSize;
// Whether to perform secondary keyframe index writing to the header after MP4 recording is completed
extern const std::string kFastStart;
// Whether to loop read the MP4 file from the beginning
extern const std::string kFileRepeat;
// Whether to use fmp4 format for MP4 recording files
extern const std::string kEnableFmp4;
// Stream name for recording motion and low-resolution streams
extern const std::string kArchiveStreamName;
} // namespace Record

// //////////HLS related configuration///////////
namespace Hls {
// HLS slice duration, in seconds
extern const std::string kSegmentDuration;
// Number of HLS slices in the m3u8 file. If set to 0, the slices will not be deleted and will be saved as on-demand
extern const std::string kSegmentNum;
// If set to 0, the slices will not be retained, if set to 1, the slices will be retained all the time
extern const std::string kSegmentKeep;
// Number of HLS slice delays. Greater than 0 will generate hls_delay.m3u8 file, 0 will not generate
extern const std::string kSegmentDelay;
// Number of HLS slices that continue to be retained on disk after being removed from the m3u8 file
extern const std::string kSegmentRetain;
// HLS file write cache size
extern const std::string kFileBufSize;
// Whether to broadcast ts slice completion notification
extern const std::string kBroadcastRecordTs;
// HLS live file deletion delay, in seconds
extern const std::string kDeleteDelaySec;
// If set to 1, the length of the first slice is forced to be 1 GOP
extern const std::string kFastRegister;
} // namespace Hls

// //////////Rtp proxy related configuration///////////
namespace RtpProxy {
// Rtp debug data save directory, empty if not generated
extern const std::string kDumpDir;
// Rtp receive timeout
extern const std::string kTimeoutSec;
// Random port range, at least 36 ports are guaranteed
// This range also limits the rtsp server udp port range
extern const std::string kPortRange;
// Rtp server h264 pt
extern const std::string kH264PT;
// Rtp server h265 pt
extern const std::string kH265PT;
// Rtp server ps pt
extern const std::string kPSPT;
// Rtp server opus pt
extern const std::string kOpusPT;
// Whether to enable gop cache optimization cascade second-open experience for startSendRtp/startRecord related functions, enabled by default, and cached 1 GOP
extern const std::string kGopCache;
// When sending g711 rtp packets in national standard, what is the duration of each packet, the default is 100 ms, the range is 20~180ms (gb28181-2016, c.2.4),
// It is best to be a multiple of 20, the program automatically rounds to the nearest multiple of 20
extern const std::string kRtpG711DurMs;
// udp recv socket buffer size
extern const std::string kUdpRecvSocketBuffer;
// Wait for the next frame after ps/ts parsing to determine whether the frame is complete. After turning it on, it will improve compatibility, but it may increase the delay.
extern const std::string kMergeFrame;
} // namespace RtpProxy

/**
 * Rtsp/rtmp player, pusher related settings name,
 * These settings are not used in the configuration file
 * Only used to set a specific player or pusher instance
 */
namespace Client {
// Specify network card ip
extern const std::string kNetAdapter;
// Set rtp transport type, options are 0 (tcp, default), 1 (udp), 2 (multicast)
// Set method: player[PlayerBase::kRtpType] = 0/1/2;
extern const std::string kRtpType;
// Whether the RTSP player sends signaling heartbeat or RTCP heartbeat, options are 0 (both), 1 (RTCP heartbeat), 2 (signaling heartbeat)
// Set method: player[PlayerBase::kRtspBeatType] = 0/1/2;
extern const std::string kRtspBeatType;
// RTSP authentication username
extern const std::string kRtspUser;
// RTSP authentication user password, can be plain text or MD5, MD5 password generation method md5(username:realm:password)
extern const std::string kRtspPwd;
// Whether the RTSP authentication user password is MD5 type
extern const std::string kRtspPwdIsMD5;
// Handshake timeout, default 10,000 milliseconds
extern const std::string kTimeoutMS;
// RTP/RTMP packet receive timeout, default 5000 seconds
extern const std::string kMediaTimeoutMS;
// RTSP/RTMP heartbeat time, default 5000 milliseconds
extern const std::string kBeatIntervalMS;
// Whether it is performance test mode, performance test mode will not parse RTP or RTMP packets after being turned on
extern const std::string kBenchmarkMode;
// Whether the player waits for all tracks to be ready before calling back when triggering the playback success event
extern const std::string kWaitTrackReady;
// RTSP playback specified track, options are 0 (not specified, default), 1 (video), 2 (audio)
// Set method: player[Client::kPlayTrack] = 0/1/2;
extern const std::string kPlayTrack;
// Set proxy url, currently only supports http protocol
extern const std::string kProxyUrl;
// Set the start RTSP playback speed
extern const std::string kRtspSpeed;
// Set SRT delay
extern const std::string kLatency;
// Set SRT PassPhrase
extern const std::string kPassPhrase;
// Custom rtsp/http header
extern const std::string kCustomHeader;
} // namespace Client

// //////////SSDP configuration///////////
namespace Ssdp {
// SSDP timeout time
extern const std::string kTimeOutSec;
// Multicast allocation address
extern const std::string kAddrMulticast;
} //namespace SSDP

/////////////Motion detection configuration///////////
namespace Motion {
// Sensitivity of motion detection, the larger the value, the more sensitive
extern const std::string kSensitivity;
// Minimum interval time for motion detection trigger, in milliseconds
extern const std::string kIntervalMS;
// Minimum duration for motion detection to be considered valid, in milliseconds
extern const std::string kMinDurationMS;
// Region of interest rows for motion detection
extern const std::string kROIRows;
// Region of interest columns for motion detection
extern const std::string kROICols;
// Default region of interest level for motion detection
extern const std::string kROIDefaultLevel;
// Whether to use the Y channel for motion detection
extern const std::string kUseYChannel;
// Whether to save image when motion is detected, the image is saved in the same directory as the MP4 recording file, and the file name is "motion_yyyymmdd_hhmmss.jpg".
// Using for debug purpose, it may cause performance issues when enabled, so it is disabled by default.
extern const std::string kSaveImage;
// Aggregation window for MotionSummaryBlock generation (ms). Default: 10 seconds.
extern const std::string kSummaryWindowMS;
} // namespace Motion

} // namespace mediakit

#endif /* COMMON_CONFIG_H */
