#ifndef MK_EVENTS_H
#define MK_EVENTS_H

#include "mk_common.h"
#include "mk_events_objects.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /**
     * Register or unregister MediaSource event broadcast
     * @param regist Register as 1, unregister as 0
     * @param sender The MediaSource object
     */
    void (API_CALL *on_mk_media_changed)(int regist,
                                         const mk_media_source sender);

    /**
     * Receive rtsp/rtmp push stream event broadcast, control push stream authentication through this event
     * @see mk_publish_auth_invoker_do
     * @param url_info Push stream url related information
     * @param invoker Execute invoker to return authentication result
     * @param sender The tcp client related information
     */
    void (API_CALL *on_mk_media_publish)(const mk_media_info url_info,
                                         const mk_publish_auth_invoker invoker,
                                         const mk_sock_info sender);

    /**
     * Play rtsp/rtmp/http-flv/hls event broadcast, control playback authentication through this event
     * @see mk_auth_invoker_do
     * @param url_info Play url related information
     * @param invoker Execute invoker to return authentication result
     * @param sender Play client related information
     */
    void (API_CALL *on_mk_media_play)(const mk_media_info url_info,
                                      const mk_auth_invoker invoker,
                                      const mk_sock_info sender);

    /**
     * This event will be broadcast after the stream is not found. Please pull the stream or other methods to generate the stream after listening to this event, so that you can pull the stream on demand.
     * @param url_info Play url related information
     * @param sender Play client related information
     * @return 1 Close directly
     *         0 Wait for stream registration
     */
    int (API_CALL *on_mk_media_not_found)(const mk_media_info url_info,
                                           const mk_sock_info sender);

    /**
     * Triggered when a stream is not consumed by anyone, the purpose is to achieve business logic such as actively disconnecting the pull stream when no one is watching
     * @param sender The MediaSource object
     */
    void (API_CALL *on_mk_media_no_reader)(const mk_media_source sender);

    /**
     * Receive http api request broadcast (including GET/POST)
     * @param parser Http request content object
     * @param invoker Execute this invoker to return http reply
     * @param consumed Set to 1 if we want to handle this event
     * @param sender Http client related information
     */
    void (API_CALL *on_mk_http_request)(const mk_parser parser,
                                        const mk_http_response_invoker invoker,
                                        int *consumed,
                                        const mk_sock_info sender);

    /**
     * In the http file server, receive the broadcast of http access to files or directories, control the access permission of the http directory through this event
     * @param parser Http request content object
     * @param path File absolute path
     * @param is_dir Whether path is a folder
     * @param invoker Execute invoker to return the result of accessing the file this time
     * @param sender Http client related information
     */
    void (API_CALL *on_mk_http_access)(const mk_parser parser,
                                       const char *path,
                                       int is_dir,
                                       const mk_http_access_path_invoker invoker,
                                       const mk_sock_info sender);

    /**
     * In the http file server, receive the broadcast before http access to files or directories, you can control the mapping of http url to file path through this event
     * By overriding the path parameter in this event, you can achieve the purpose of selecting different http root directories according to virtual hosts or apps
     * @param parser Http request content object
     * @param path File absolute path, overriding it can redirect to other files
     * @param sender Http client related information
     */
    void (API_CALL *on_mk_http_before_access)(const mk_parser parser,
                                              char *path,
                                              const mk_sock_info sender);

    /**
     * Does this rtsp stream need authentication? If so, call invoker and pass in realm, otherwise pass in empty realm
     * @param url_info Request rtsp url related information
     * @param invoker Execute invoker to return whether rtsp exclusive authentication is required
     * @param sender Rtsp client related information
     */
    void (API_CALL *on_mk_rtsp_get_realm)(const mk_media_info url_info,
                                          const mk_rtsp_get_realm_invoker invoker,
                                          const mk_sock_info sender);

    /**
     * Request authentication user password event, user_name is the username, must_no_encrypt if true, then you must provide plain text password (because it is base64 authentication method at this time), otherwise it will lead to authentication failure
     * After getting the password, please call invoker and input the corresponding type of password and password type, invoker will match the password when executing
     * @param url_info Request rtsp url related information
     * @param realm Rtsp authentication realm
     * @param user_name Rtsp authentication username
     * @param must_no_encrypt If true, then you must provide plain text password (because it is base64 authentication method at this time), otherwise it will lead to authentication failure
     * @param invoker  Execute invoker to return the password of rtsp exclusive authentication
     * @param sender Rtsp client information
     */
    void (API_CALL *on_mk_rtsp_auth)(const mk_media_info url_info,
                                     const char *realm,
                                     const char *user_name,
                                     int must_no_encrypt,
                                     const mk_rtsp_auth_invoker invoker,
                                     const mk_sock_info sender);

    /**
     * Broadcast after recording mp4 fragment file successfully
     */
    void (API_CALL *on_mk_record_mp4)(const mk_record_info mp4);

    /**
     * Broadcast after recording mkv fragment file successfully
     */
    void (API_CALL *on_mk_record_mkv)(const mk_record_info mkv);

     /**
      * Broadcast after recording ts fragment file successfully
     */
    void (API_CALL *on_mk_record_ts)(const mk_record_info ts);

    /**
     * Shell login authentication
     */
    void (API_CALL *on_mk_shell_login)(const char *user_name,
                                       const char *passwd,
                                       const mk_auth_invoker invoker,
                                       const mk_sock_info sender);

    /**
     * Stop rtsp/rtmp/http-flv session after traffic reporting event broadcast
     * @param url_info Play url related information
     * @param total_bytes Total traffic consumed up and down, unit bytes
     * @param total_seconds The duration of this tcp session, unit seconds
     * @param is_player Whether the client is a player
     */
    void (API_CALL *on_mk_flow_report)(const mk_media_info url_info,
                                       size_t total_bytes,
                                       size_t total_seconds,
                                       int is_player,
                                       const mk_sock_info sender);


    /**
     * Log output broadcast
     * @param level Log level
     * @param file Source file name
     * @param line Source file line
     * @param function Source file function name
     * @param message Log content
     */
    void (API_CALL *on_mk_log)(int level, const char *file, int line, const char *function, const char *message);

    /**
     * Send rtp stream failure callback, applicable to rtp sending triggered by mk_media_source_start_send_rtp/mk_media_start_send_rtp interface
     * @param vhost Virtual host
     * @param app Application name
     * @param stream Stream id
     * @param ssrc Ssrc's decimal print, convert to integer through atoi
     * @param err Error code
     * @param msg Error message
     */
    void (API_CALL *on_mk_media_send_rtp_stop)(const char *vhost, const char *app, const char *stream, const char *ssrc, int err, const char *msg);

    /**
     * Rtc sctp connection in/complete/failure/close callback
     * @param rtc_transport Data channel object
     */
    void (API_CALL *on_mk_rtc_sctp_connecting)(mk_rtc_transport rtc_transport);
    void (API_CALL *on_mk_rtc_sctp_connected)(mk_rtc_transport rtc_transport);
    void (API_CALL *on_mk_rtc_sctp_failed)(mk_rtc_transport rtc_transport);
    void (API_CALL *on_mk_rtc_sctp_closed)(mk_rtc_transport rtc_transport);

    /**
     * Rtc data channel send data callback
     * @param rtc_transport Data channel object
     * @param msg Data
     * @param len Data length
     */
    void (API_CALL *on_mk_rtc_sctp_send)(mk_rtc_transport rtc_transport, const uint8_t *msg, size_t len);

    /**
     * Rtc data channel receive data callback
     * @param rtc_transport Data channel object
     * @param streamId Stream id
     * @param ppid Protocol id
     * @param msg Data
     * @param len Data length
     */
    void (API_CALL *on_mk_rtc_sctp_received)(mk_rtc_transport rtc_transport, uint16_t streamId, uint32_t ppid, const uint8_t *msg, size_t len);

} mk_events;


/**
 * Listen to events in S3MediaKit
 * @param events The structure of each event, this object will be copied again internally, it can be set to null to cancel listening
 */
API_EXPORT void API_CALL mk_events_listen(const mk_events *events);


#ifdef __cplusplus
}
#endif
#endif //MK_EVENTS_H
