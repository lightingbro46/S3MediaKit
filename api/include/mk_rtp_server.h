#include "mk_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mk_rtp_server_t *mk_rtp_server;

/**
 * Create GB28181 RTP server
 * @param port Listening port, 0 for random
 * @param tcp_mode tcp mode (0: not listening to port 1: listening to port 2: actively connect to the server)
 * @param stream_id Stream id bound to this port
 * @return
 */
API_EXPORT mk_rtp_server API_CALL mk_rtp_server_create(uint16_t port, int tcp_mode, const char *stream_id);
API_EXPORT mk_rtp_server API_CALL mk_rtp_server_create2(uint16_t port, int tcp_mode, const char *vhost, const char *app, const char *stream_id);

/**
 * Callback for whether the connection to the server is successful in TCP active mode
 */
typedef void(API_CALL *on_mk_rtp_server_connected)(void *user_data, int err, const char *what, int sys_err);

/**
 * Connect to the server in TCP active mode
 * @param @param ctx Server object
 * @param dst_url Server address
 * @param dst_port Server port
 * @param cb Callback for whether the connection to the server is successful
 * @param user_data User data pointer
 * @return
 */
API_EXPORT void API_CALL mk_rtp_server_connect(mk_rtp_server ctx, const char *dst_url, uint16_t dst_port, on_mk_rtp_server_connected cb, void *user_data);
API_EXPORT void API_CALL mk_rtp_server_connect2(mk_rtp_server ctx, const char *dst_url, uint16_t dst_port, on_mk_rtp_server_connected cb, void *user_data, on_user_data_free user_data_free);

/**
 * Destroy GB28181 RTP server
 * @param ctx Server object
 */
API_EXPORT void API_CALL mk_rtp_server_release(mk_rtp_server ctx);

/**
 * Get the local listening port number
 * @param ctx Server object
 * @return Port number
 */
API_EXPORT uint16_t API_CALL mk_rtp_server_port(mk_rtp_server ctx);

/**
 * Triggered when the GB28181 RTP server receives a stream timeout
 * @param user_data User data pointer
 */
typedef void(API_CALL *on_mk_rtp_server_detach)(void *user_data);

/**
 * Listen for B28181 RTP server receiving stream timeout events
 * @param ctx Server object
 * @param cb Callback function
 * @param user_data Callback function user data pointer
 */
API_EXPORT void API_CALL mk_rtp_server_set_on_detach(mk_rtp_server ctx, on_mk_rtp_server_detach cb, void *user_data);
API_EXPORT void API_CALL mk_rtp_server_set_on_detach2(mk_rtp_server ctx, on_mk_rtp_server_detach cb, void *user_data, on_user_data_free user_data_free);

#ifdef __cplusplus
}
#endif