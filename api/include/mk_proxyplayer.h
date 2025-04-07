#ifndef MK_PROXY_PLAYER_H_
#define MK_PROXY_PLAYER_H_

#include "mk_common.h"
#include "mk_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mk_proxy_player_t *mk_proxy_player;

/**
 * Create a proxy player
 * @param vhost Virtual host name, generally __defaultVhost__
 * @param app Application name
 * @param stream Stream name
 * @param rtp_type rtsp playback method: RTP_TCP = 0, RTP_UDP = 1, RTP_MULTICAST = 2
 * @param hls_enabled Whether to generate hls
 * @param mp4_enabled Whether to generate mp4
 * @return Object pointer
 */
API_EXPORT mk_proxy_player API_CALL mk_proxy_player_create(const char *vhost, const char *app, const char *stream, int hls_enabled, int mp4_enabled);


/**
 * Create a proxy player
 * @param vhost Virtual host name, generally __defaultVhost__
 * @param app Application name
 * @param stream Stream name
 * @param option ProtocolOption related configuration
 * @return Object pointer
 */
API_EXPORT mk_proxy_player API_CALL mk_proxy_player_create2(const char *vhost, const char *app, const char *stream, mk_ini option);


/**
 * Create a proxy player
 * @param vhost Virtual host name, generally __defaultVhost__
 * @param app Application name
 * @param stream Stream name
 * @param rtp_type rtsp playback method: RTP_TCP = 0, RTP_UDP = 1, RTP_MULTICAST = 2
 * @param hls_enabled Whether to generate hls
 * @param mp4_enabled Whether to generate mp4
 * @param retry_count Retry count, when <0 retry infinitely
 * @return Object pointer
 */
API_EXPORT mk_proxy_player API_CALL mk_proxy_player_create3(const char *vhost, const char *app, const char *stream, int hls_enabled, int mp4_enabled, int retry_count);


/**
 * Create a proxy player
 * @param vhost Virtual host name, generally __defaultVhost__
 * @param app Application name
 * @param stream Stream name
 * @param option ProtocolOption related configuration
 * @param retry_count Retry count, when <0 retry infinitely
 * @return Object pointer
 */
API_EXPORT mk_proxy_player API_CALL mk_proxy_player_create4(const char *vhost, const char *app, const char *stream, mk_ini option, int retry_count);


/**
 * Destroy the proxy player
 * @param ctx Object pointer
 */
API_EXPORT void API_CALL mk_proxy_player_release(mk_proxy_player ctx);

/**
 * Set proxy player configuration options
 * @param ctx Proxy player pointer
 * @param key Configuration item key, supports net_adapter/rtp_type/rtsp_user/rtsp_pwd/protocol_timeout_ms/media_timeout_ms/beat_interval_ms/rtsp_speed
 * @param val Configuration item value, if it is an integer, it needs to be converted to a unified string
 */
API_EXPORT void API_CALL mk_proxy_player_set_option(mk_proxy_player ctx, const char *key, const char *val);

/**
 * Start playback
 * @param ctx Object pointer
 * @param url Playback url, supports rtsp/rtmp
 */
API_EXPORT void API_CALL mk_proxy_player_play(mk_proxy_player ctx, const char *url);

/**
 * MediaSource.close() callback event
 * When you choose to close an associated MediaSource, it will eventually trigger this callback
 * You should call mk_proxy_player_release function through this event and release other resources
 * If you do not call mk_proxy_player_release function, then MediaSource.close() operation will be invalid
 * @param user_data User data pointer, set by mk_proxy_player_set_on_close function
 */
typedef void(API_CALL *on_mk_proxy_player_cb)(void *user_data, int err, const char *what, int sys_err);
// Keep compatible
#define on_mk_proxy_player_close on_mk_proxy_player_cb

/**
 * Listen for MediaSource.close() event
 * When you choose to close an associated MediaSource, it will eventually trigger this callback
 * You should call mk_proxy_player_release function through this event and release other resources
 * @param ctx Object pointer
 * @param cb Callback pointer
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_proxy_player_set_on_close(mk_proxy_player ctx, on_mk_proxy_player_cb cb, void *user_data);
API_EXPORT void API_CALL mk_proxy_player_set_on_close2(mk_proxy_player ctx, on_mk_proxy_player_cb cb, void *user_data, on_user_data_free user_data_free);

/**
 * Set the proxy's first playback result callback. If the first playback fails, it can be considered a startup failure.
 * @param ctx Object pointer
 * @param cb Callback pointer
 * @param user_data User data pointer
 * @param user_data_free User data release callback
 */
API_EXPORT void API_CALL mk_proxy_player_set_on_play_result(mk_proxy_player ctx, on_mk_proxy_player_cb cb, void *user_data, on_user_data_free user_data_free);

/**
 * Get the total number of viewers
 * @param ctx Object pointer
 * @return Number of viewers
 */
API_EXPORT int API_CALL mk_proxy_player_total_reader_count(mk_proxy_player ctx);

#ifdef __cplusplus
}
#endif

#endif /* MK_PROXY_PLAYER_H_ */
