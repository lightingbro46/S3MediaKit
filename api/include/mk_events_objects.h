#ifndef MK_EVENT_OBJECTS_H
#define MK_EVENT_OBJECTS_H
#include "mk_common.h"
#include "mk_tcp.h"
#include "mk_track.h"
#include "mk_util.h"
#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////RecordInfo/////////////////////////////////////////////
// RecordInfo object's C mapping
typedef struct mk_record_info_t *mk_record_info;
// GMT standard time, unit is seconds
API_EXPORT uint64_t API_CALL mk_record_info_get_start_time(const mk_record_info ctx);
// Recording length, unit is seconds
API_EXPORT float API_CALL mk_record_info_get_time_len(const mk_record_info ctx);
// File size, unit is BYTE
API_EXPORT size_t API_CALL mk_record_info_get_file_size(const mk_record_info ctx);
// File path
API_EXPORT const char *API_CALL mk_record_info_get_file_path(const mk_record_info ctx);
// File name
API_EXPORT const char *API_CALL mk_record_info_get_file_name(const mk_record_info ctx);
// Folder path
API_EXPORT const char *API_CALL mk_record_info_get_folder(const mk_record_info ctx);
// Playback path
API_EXPORT const char *API_CALL mk_record_info_get_url(const mk_record_info ctx);
// Application name
API_EXPORT const char *API_CALL mk_record_info_get_vhost(const mk_record_info ctx);
// Stream ID
API_EXPORT const char *API_CALL mk_record_info_get_app(const mk_record_info ctx);
// Virtual host
API_EXPORT const char *API_CALL mk_record_info_get_stream(const mk_record_info ctx);

// // The following macros ensure user code compatibility, binary abi is incompatible, users need to recompile and link /////
#define mk_mp4_info mk_record_info
#define mk_mp4_info_get_start_time mk_record_info_get_start_time
#define mk_mp4_info_get_time_len mk_record_info_get_time_len
#define mk_mp4_info_get_file_size mk_record_info_get_file_size
#define mk_mp4_info_get_file_path mk_record_info_get_file_path
#define mk_mp4_info_get_file_name mk_record_info_get_file_name
#define mk_mp4_info_get_folder mk_record_info_get_folder
#define mk_mp4_info_get_url mk_record_info_get_url
#define mk_mp4_info_get_vhost mk_record_info_get_vhost
#define mk_mp4_info_get_app mk_record_info_get_app
#define mk_mp4_info_get_stream mk_record_info_get_stream

///////////////////////////////////////////Parser/////////////////////////////////////////////
// Parser object's C mapping
typedef struct mk_parser_t *mk_parser;
// Parser object's Headers foreach callback
typedef void(API_CALL *on_mk_parser_header_cb)(void *user_data, const char *key, const char *val);
// Parser::Method(), get the command word, such as GET/POST
API_EXPORT const char* API_CALL mk_parser_get_method(const mk_parser ctx);
// Parser::Url(), get the HTTP access url (excluding the parameters after ?)
API_EXPORT const char* API_CALL mk_parser_get_url(const mk_parser ctx);
// Parser::Params(), the parameter string after ?
API_EXPORT const char* API_CALL mk_parser_get_url_params(const mk_parser ctx);
// Parser::getUrlArgs()["key"], get the specific parameter in the parameters after ?
API_EXPORT const char* API_CALL mk_parser_get_url_param(const mk_parser ctx,const char *key);
// Parser::Tail(), get protocol related information, such as HTTP/1.1
API_EXPORT const char* API_CALL mk_parser_get_tail(const mk_parser ctx);
// Parser::getValues()["key"], get the specific field in the HTTP header
API_EXPORT const char* API_CALL mk_parser_get_header(const mk_parser ctx,const char *key);
// Parser::Content(), get the HTTP body
API_EXPORT const char* API_CALL mk_parser_get_content(const mk_parser ctx, size_t *length);
// Loop to get all headers
API_EXPORT void API_CALL mk_parser_headers_for_each(const mk_parser ctx, on_mk_parser_header_cb cb, void *user_data);

///////////////////////////////////////////MediaInfo/////////////////////////////////////////////
// MediaInfo object's C mapping
typedef struct mk_media_info_t *mk_media_info;
//MediaInfo::param_strs
API_EXPORT const char* API_CALL mk_media_info_get_params(const mk_media_info ctx);
//MediaInfo::schema
API_EXPORT const char* API_CALL mk_media_info_get_schema(const mk_media_info ctx);
//MediaInfo::vhost
API_EXPORT const char* API_CALL mk_media_info_get_vhost(const mk_media_info ctx);
//MediaInfo::app
API_EXPORT const char* API_CALL mk_media_info_get_app(const mk_media_info ctx);
//MediaInfo::stream
API_EXPORT const char* API_CALL mk_media_info_get_stream(const mk_media_info ctx);
//MediaInfo::host
API_EXPORT const char* API_CALL mk_media_info_get_host(const mk_media_info ctx);
//MediaInfo::port
API_EXPORT uint16_t API_CALL mk_media_info_get_port(const mk_media_info ctx);


///////////////////////////////////////////MediaSource/////////////////////////////////////////////
// MediaSource object's C mapping
typedef struct mk_media_source_t *mk_media_source;
// Callback function to find MediaSource
typedef void(API_CALL *on_mk_media_source_find_cb)(void *user_data, const mk_media_source ctx);

//MediaSource::getSchema()
API_EXPORT const char* API_CALL mk_media_source_get_schema(const mk_media_source ctx);
//MediaSource::getVhost()
API_EXPORT const char* API_CALL mk_media_source_get_vhost(const mk_media_source ctx);
//MediaSource::getApp()
API_EXPORT const char* API_CALL mk_media_source_get_app(const mk_media_source ctx);
//MediaSource::getId()
API_EXPORT const char* API_CALL mk_media_source_get_stream(const mk_media_source ctx);
//MediaSource::readerCount()
API_EXPORT int API_CALL mk_media_source_get_reader_count(const mk_media_source ctx);
//MediaSource::totalReaderCount()
API_EXPORT int API_CALL mk_media_source_get_total_reader_count(const mk_media_source ctx);
// get track count from MediaSource
API_EXPORT int API_CALL mk_media_source_get_track_count(const mk_media_source ctx);
// copy track reference by index from MediaSource, please use mk_track_unref to release it
API_EXPORT mk_track API_CALL mk_media_source_get_track(const mk_media_source ctx, int index);
// MediaSource::Track:loss
API_EXPORT float API_CALL mk_media_source_get_track_loss(const mk_media_source ctx, const mk_track track);
// MediaSource::broadcastMessage
API_EXPORT int API_CALL mk_media_source_broadcast_msg(const mk_media_source ctx, const char *msg, size_t len);
// MediaSource::getOriginUrl()
API_EXPORT const char* API_CALL mk_media_source_get_origin_url(const mk_media_source ctx);
// MediaSource::getOriginType()
API_EXPORT int API_CALL mk_media_source_get_origin_type(const mk_media_source ctx);
// MediaSource::getOriginTypeStr(), please use mk_free to release the return value after use
API_EXPORT const char *API_CALL mk_media_source_get_origin_type_str(const mk_media_source ctx);
// MediaSource::getCreateStamp()
API_EXPORT uint64_t API_CALL mk_media_source_get_create_stamp(const mk_media_source ctx);
// MediaSource::isRecording()  0:hls,1:MP4
API_EXPORT int API_CALL mk_media_source_is_recording(const mk_media_source ctx, int type);
// MediaSource::getBytesSpeed() 
API_EXPORT int API_CALL mk_media_source_get_bytes_speed(const mk_media_source ctx);
// MediaSource::getAliveSecond()
API_EXPORT uint64_t API_CALL mk_media_source_get_alive_second(const mk_media_source ctx);
/**
 * Live sources are called MediaSource in S3MediaKit,
 * Currently, there are 3 types, namely RtmpMediaSource, RtspMediaSource, HlsMediaSource
 * The source is generated in both passive and active ways:
 * Passive ways are rtsp/rtmp/rtp push stream, mp4 on-demand
 * Active ways include objects created by mk_media_create (DevChannel), objects created by mk_proxy_player_create (PlayerProxy)
 * You don't need to do anything for passive ways, S3MediaKit has already adapted the MediaSource::close() event by default, which will close the live stream
 * For active ways, you need to set the callback of this event, you need to choose to delete the object yourself
 * You can set the callback through the mk_proxy_player_set_on_close and mk_media_set_on_close functions,
 * Please delete the object in the callback to complete the media closure, otherwise why call the mk_media_source_close function?
 * @param ctx object
 * @param force Whether to force closure, if forced closure, it will be closed even if someone is watching
 * @return 0 means failure, 1 means success
 */
API_EXPORT int API_CALL mk_media_source_close(const mk_media_source ctx,int force);
//MediaSource::seekTo()
API_EXPORT int API_CALL mk_media_source_seek_to(const mk_media_source ctx,uint32_t stamp);

/**
 * Callback for whether rtp push stream is successful or not (after the first success, it will keep retrying)
 */
typedef void(API_CALL *on_mk_media_source_send_rtp_result)(void *user_data, uint16_t local_port, int err, const char *msg);

// MediaSource::startSendRtp, please refer to mk_media_start_send_rtp, note that the ctx parameter type is different
API_EXPORT void API_CALL mk_media_source_start_send_rtp(const mk_media_source ctx, const char *dst_url, uint16_t dst_port, const char *ssrc, int con_type, on_mk_media_source_send_rtp_result cb, void *user_data);
API_EXPORT void API_CALL mk_media_source_start_send_rtp2(const mk_media_source ctx, const char *dst_url, uint16_t dst_port, const char *ssrc, int con_type, on_mk_media_source_send_rtp_result cb, void *user_data, on_user_data_free user_data_free);
API_EXPORT void API_CALL mk_media_source_start_send_rtp3(const mk_media_source ctx, const char *dst_url, uint16_t dst_port, const char *ssrc, int con_type, mk_ini options, on_mk_media_source_send_rtp_result cb, void *user_data);
API_EXPORT void API_CALL mk_media_source_start_send_rtp4(const mk_media_source ctx, const char *dst_url, uint16_t dst_port, const char *ssrc, int con_type, mk_ini options, on_mk_media_source_send_rtp_result cb,void *user_data, on_user_data_free user_data_free);
// MediaSource::stopSendRtp, please refer to mk_media_stop_send_rtp, note that the ctx parameter type is different
API_EXPORT int API_CALL mk_media_source_stop_send_rtp(const mk_media_source ctx);

//MediaSource::find()
API_EXPORT void API_CALL mk_media_source_find(const char *schema,
                                              const char *vhost,
                                              const char *app,
                                              const char *stream,
                                              int from_mp4,
                                              void *user_data,
                                              on_mk_media_source_find_cb cb);

API_EXPORT mk_media_source API_CALL mk_media_source_find2(const char *schema,
                                                          const char *vhost,
                                                          const char *app,
                                                          const char *stream,
                                                          int from_mp4);
//MediaSource::for_each_media()
API_EXPORT void API_CALL mk_media_source_for_each(void *user_data, on_mk_media_source_find_cb cb, const char *schema,
                                                  const char *vhost, const char *app, const char *stream);

///////////////////////////////////////////HttpBody/////////////////////////////////////////////
// cpp
//HttpBody C mapping of object
typedef struct mk_http_body_t *mk_http_body;
/**
 * Generate HttpStringBody
 * @param str String pointer
 * @param len String length, if it is 0, use strlen to get it
 */
API_EXPORT mk_http_body API_CALL mk_http_body_from_string(const char *str,size_t len);

/**
 * Generate HttpBufferBody
 * @param buffer mk_buffer object
 */
API_EXPORT mk_http_body API_CALL mk_http_body_from_buffer(mk_buffer buffer);


/**
 * Generate HttpFileBody
 * @param file_path File full path
 */
API_EXPORT mk_http_body API_CALL mk_http_body_from_file(const char *file_path);

/**
 * Generate HttpMultiFormBody
 * @param key_val Parameter key-value
 * @param file_path File full path
 */
API_EXPORT mk_http_body API_CALL mk_http_body_from_multi_form(const char *key_val[],const char *file_path);

/**
 * Destroy HttpBody
 */
API_EXPORT void API_CALL mk_http_body_release(mk_http_body ctx);

///////////////////////////////////////////HttpResponseInvoker/////////////////////////////////////////////
// HttpSession::HttpResponseInvoker C map of object
typedef struct mk_http_response_invoker_t *mk_http_response_invoker;

/**
 * HttpSession::HttpResponseInvoker(const string &codeOut, const StrCaseMap &headerOut, const HttpBody::Ptr &body);
 * @param response_code For example 200
 * @param response_header The returned http header, for example {"Content-Type","text/html",NULL} must end with NULL
 * @param response_body Body object
 */
API_EXPORT void API_CALL mk_http_response_invoker_do(const mk_http_response_invoker ctx,
                                                     int response_code,
                                                     const char **response_header,
                                                     const mk_http_body response_body);

/**
 * HttpSession::HttpResponseInvoker(const string &codeOut, const StrCaseMap &headerOut, const string &body);
 * @param response_code For example 200
 * @param response_header The returned http header, for example {"Content-Type","text/html",NULL} must end with NULL
 * @param response_content The returned content part, for example a web page content
 */
API_EXPORT void API_CALL mk_http_response_invoker_do_string(const mk_http_response_invoker ctx,
                                                            int response_code,
                                                            const char **response_header,
                                                            const char *response_content);
/**
 * HttpSession::HttpResponseInvoker(const StrCaseMap &requestHeader,const StrCaseMap &responseHeader,const string &filePath);
 * @param request_parser The mk_parser object in the request event, used to extract the Range field in the http header, use this field to fseek first and then send the file part fragment
 * @param response_header The returned http header, for example {"Content-Type","text/html",NULL} must end with NULL
 * @param response_file_path The returned content part, for example /path/to/html/file
 */
API_EXPORT void API_CALL mk_http_response_invoker_do_file(const mk_http_response_invoker ctx,
                                                          const mk_parser request_parser,
                                                          const char *response_header[],
                                                          const char *response_file_path);
/**
 * Clone the mk_http_response_invoker object, by cloning the object to a heap object, you can achieve cross-thread asynchronous execution of mk_http_response_invoker_do
 * If you execute mk_http_response_invoker_do synchronously, then there is no need to clone the object
*/
API_EXPORT mk_http_response_invoker API_CALL mk_http_response_invoker_clone(const mk_http_response_invoker ctx);

/**
 * Destroy the cloned object on the heap
 */
API_EXPORT void API_CALL mk_http_response_invoker_clone_release(const mk_http_response_invoker ctx);

///////////////////////////////////////////HttpAccessPathInvoker/////////////////////////////////////////////
// HttpSession::HttpAccessPathInvoker C map of object
typedef struct mk_http_access_path_invoker_t *mk_http_access_path_invoker;

/**
 * HttpSession::HttpAccessPathInvoker(const string &errMsg,const string &accessPath, int cookieLifeSecond);
 * @param err_msg If it is empty, it means that the authentication is passed, otherwise it is an error prompt, it can be null
 * @param access_path The root directory to run or prohibit access, it can be null
 * @param cookie_life_second Authentication cookie validity period
 **/
API_EXPORT void API_CALL mk_http_access_path_invoker_do(const mk_http_access_path_invoker ctx,
                                                        const char *err_msg,
                                                        const char *access_path,
                                                        int cookie_life_second);

/**
 * Clone the mk_http_access_path_invoker object, by cloning the object to a heap object, you can achieve cross-thread asynchronous execution of mk_http_access_path_invoker_do
 * If you execute mk_http_access_path_invoker_do synchronously, then there is no need to clone the object
*/
API_EXPORT mk_http_access_path_invoker API_CALL mk_http_access_path_invoker_clone(const mk_http_access_path_invoker ctx);

/**
 * Destroy the cloned object on the heap
 */
API_EXPORT void API_CALL mk_http_access_path_invoker_clone_release(const mk_http_access_path_invoker ctx);

///////////////////////////////////////////RtspSession::onGetRealm/////////////////////////////////////////////
// RtspSession::onGetRealm C map of object
typedef struct mk_rtsp_get_realm_invoker_t *mk_rtsp_get_realm_invoker;
/**
 * Execute RtspSession::onGetRealm
 * @param realm Whether this rtsp stream needs to enable rtsp exclusive authentication, to null or empty string does not authenticate
 */
API_EXPORT void API_CALL mk_rtsp_get_realm_invoker_do(const mk_rtsp_get_realm_invoker ctx,
                                                      const char *realm);

/**
 * Clone the mk_rtsp_get_realm_invoker object, by cloning the object to a heap object, you can achieve cross-thread asynchronous execution of mk_rtsp_get_realm_invoker_do
 * If you execute mk_rtsp_get_realm_invoker_do synchronously, then there is no need to clone the object
*/
API_EXPORT mk_rtsp_get_realm_invoker API_CALL mk_rtsp_get_realm_invoker_clone(const mk_rtsp_get_realm_invoker ctx);

/**
 * Destroy the cloned object on the heap
 */
API_EXPORT void API_CALL mk_rtsp_get_realm_invoker_clone_release(const mk_rtsp_get_realm_invoker ctx);

///////////////////////////////////////////RtspSession::onAuth/////////////////////////////////////////////
// RtspSession::onAuth C map of object
typedef struct mk_rtsp_auth_invoker_t *mk_rtsp_auth_invoker;

/**
 * Execute RtspSession::onAuth
 * @param encrypted If true, it means that the password is md5 encrypted, otherwise it is plain text password, if you provide md5 password when requesting plain text password, it will cause authentication failure
 * @param pwd_or_md5 Plain text password or md5 encrypted password
 */
API_EXPORT void API_CALL mk_rtsp_auth_invoker_do(const mk_rtsp_auth_invoker ctx,
                                                 int encrypted,
                                                 const char *pwd_or_md5);

/**
 * Clone the mk_rtsp_auth_invoker object, by cloning the object to a heap object, you can achieve cross-thread asynchronous execution of mk_rtsp_auth_invoker_do
 * If you execute mk_rtsp_auth_invoker_do synchronously, then there is no need to clone the object
 */
API_EXPORT mk_rtsp_auth_invoker API_CALL mk_rtsp_auth_invoker_clone(const mk_rtsp_auth_invoker ctx);

/**
 * Destroy the cloned object on the heap
 */
API_EXPORT void API_CALL mk_rtsp_auth_invoker_clone_release(const mk_rtsp_auth_invoker ctx);

///////////////////////////////////////////Broadcast::PublishAuthInvoker/////////////////////////////////////////////
// Broadcast::PublishAuthInvoker C map of object
typedef struct mk_publish_auth_invoker_t *mk_publish_auth_invoker;

/**
 * Execute Broadcast::PublishAuthInvoker
 * @param err_msg Empty or null means authentication success
 * @param enable_hls Whether to allow hls conversion
 * @param enable_mp4 Whether to allow MP4 recording
 */
API_EXPORT void API_CALL mk_publish_auth_invoker_do(const mk_publish_auth_invoker ctx,
                                                    const char *err_msg,
                                                    int enable_hls,
                                                    int enable_mp4);

API_EXPORT void API_CALL mk_publish_auth_invoker_do2(const mk_publish_auth_invoker ctx, const char *err_msg, mk_ini option);

/**
 * Clone the mk_publish_auth_invoker object, by cloning the object to a heap object, you can achieve cross-thread asynchronous execution of mk_publish_auth_invoker_do
 * If you execute mk_publish_auth_invoker_do synchronously, then there is no need to clone the object
 */
API_EXPORT mk_publish_auth_invoker API_CALL mk_publish_auth_invoker_clone(const mk_publish_auth_invoker ctx);

/**
 * Destroy the cloned object on the heap
 */
API_EXPORT void API_CALL mk_publish_auth_invoker_clone_release(const mk_publish_auth_invoker ctx);

///////////////////////////////////////////Broadcast::AuthInvoker/////////////////////////////////////////////
// Broadcast::AuthInvoker C map of object
typedef struct mk_auth_invoker_t *mk_auth_invoker;

/**
 * Execute Broadcast::AuthInvoker
 * @param err_msg Empty or null means authentication success
 */
API_EXPORT void API_CALL mk_auth_invoker_do(const mk_auth_invoker ctx, const char *err_msg);

/**
 * Clone the mk_auth_invoker object. By cloning the object to a heap object, we can achieve asynchronous execution of mk_auth_invoker_do across threads.
 * If mk_auth_invoker_do is executed synchronously, there is no need to clone the object.
 */
API_EXPORT mk_auth_invoker API_CALL mk_auth_invoker_clone(const mk_auth_invoker ctx);

/**
 * Destroy the cloned object on the heap.
 */
API_EXPORT void API_CALL mk_auth_invoker_clone_release(const mk_auth_invoker ctx);

///////////////////////////////////////////WebRtcTransport/////////////////////////////////////////////
// C mapping of the WebRtcTransport object
typedef struct mk_rtc_transport_t *mk_rtc_transport;

/**
 * Send rtc data channel
 * @param ctx Data channel object
 * @param streamId Stream id
 * @param ppid Protocol id
 * @param msg Data
 * @param len Data length
 */
API_EXPORT void API_CALL mk_rtc_send_datachannel(const mk_rtc_transport ctx, uint16_t streamId, uint32_t ppid, const char* msg, size_t len);

#ifdef __cplusplus
}
#endif
#endif //MK_EVENT_OBJECTS_H
