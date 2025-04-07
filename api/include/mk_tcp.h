#ifndef MK_TCP_H
#define MK_TCP_H

#include "mk_common.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////Buffer::Ptr/////////////////////////////////////////////

typedef struct mk_buffer_t *mk_buffer;
typedef void(API_CALL *on_mk_buffer_free)(void *user_data, void *data);

/**
 * Create a buffer object
 * @param data Data pointer
 * @param len Data length
 * @param cb Data pointer free callback function. This parameter is set to null, the data will be copied internally
 * @param user_data The first parameter of the data pointer free callback function on_mk_buffer_free
 * @return buffer object
 */
API_EXPORT mk_buffer API_CALL mk_buffer_from_char(const char *data, size_t len, on_mk_buffer_free cb, void *user_data);
API_EXPORT mk_buffer API_CALL mk_buffer_from_char2(const char *data, size_t len, on_mk_buffer_free cb, void *user_data, on_user_data_free user_data_free);
API_EXPORT mk_buffer API_CALL mk_buffer_ref(mk_buffer buffer);
API_EXPORT void API_CALL mk_buffer_unref(mk_buffer buffer);
API_EXPORT const char* API_CALL mk_buffer_get_data(mk_buffer buffer);
API_EXPORT size_t API_CALL mk_buffer_get_size(mk_buffer buffer);

///////////////////////////////////////////SockInfo/////////////////////////////////////////////
// C mapping of SockInfo object
typedef struct mk_sock_info_t *mk_sock_info;

//SockInfo::get_peer_ip()
API_EXPORT const char* API_CALL mk_sock_info_peer_ip(const mk_sock_info ctx, char *buf);
//SockInfo::get_local_ip()
API_EXPORT const char* API_CALL mk_sock_info_local_ip(const mk_sock_info ctx, char *buf);
//SockInfo::get_peer_port()
API_EXPORT uint16_t API_CALL mk_sock_info_peer_port(const mk_sock_info ctx);
//SockInfo::get_local_port()
API_EXPORT uint16_t API_CALL mk_sock_info_local_port(const mk_sock_info ctx);

#ifndef SOCK_INFO_API_RENAME
#define SOCK_INFO_API_RENAME
// Get network information after converting mk_tcp_session object to mk_sock_info object
#define mk_tcp_session_peer_ip(x,buf) mk_sock_info_peer_ip(mk_tcp_session_get_sock_info(x),buf)
#define mk_tcp_session_local_ip(x,buf) mk_sock_info_local_ip(mk_tcp_session_get_sock_info(x),buf)
#define mk_tcp_session_peer_port(x) mk_sock_info_peer_port(mk_tcp_session_get_sock_info(x))
#define mk_tcp_session_local_port(x) mk_sock_info_local_port(mk_tcp_session_get_sock_info(x))

// Get network information after converting mk_tcp_client object to mk_sock_info object
#define mk_tcp_client_peer_ip(x,buf) mk_sock_info_peer_ip(mk_tcp_client_get_sock_info(x),buf)
#define mk_tcp_client_local_ip(x,buf) mk_sock_info_local_ip(mk_tcp_client_get_sock_info(x),buf)
#define mk_tcp_client_peer_port(x) mk_sock_info_peer_port(mk_tcp_client_get_sock_info(x))
#define mk_tcp_client_local_port(x) mk_sock_info_local_port(mk_tcp_client_get_sock_info(x))
#endif
///////////////////////////////////////////TcpSession/////////////////////////////////////////////
// C mapping of TcpSession object
typedef struct mk_tcp_session_t *mk_tcp_session;
typedef struct mk_tcp_session_ref_t *mk_tcp_session_ref;

// Get the base class pointer to get its network information
API_EXPORT mk_sock_info API_CALL mk_tcp_session_get_sock_info(const mk_tcp_session ctx);

//TcpSession::safeShutdown()
API_EXPORT void API_CALL mk_tcp_session_shutdown(const mk_tcp_session ctx,int err,const char *err_msg);
//TcpSession::send()
API_EXPORT void API_CALL mk_tcp_session_send(const mk_tcp_session ctx, const char *data, size_t len);
API_EXPORT void API_CALL mk_tcp_session_send_buffer(const mk_tcp_session ctx, mk_buffer buffer);

// Switch to the thread where the object is located, then TcpSession::send()
API_EXPORT void API_CALL mk_tcp_session_send_safe(const mk_tcp_session ctx, const char *data, size_t len);
API_EXPORT void API_CALL mk_tcp_session_send_buffer_safe(const mk_tcp_session ctx, mk_buffer buffer);

// Create a strong reference to mk_tcp_session
API_EXPORT mk_tcp_session_ref API_CALL mk_tcp_session_ref_from(const mk_tcp_session ctx);
// Delete the strong reference to mk_tcp_session
API_EXPORT void mk_tcp_session_ref_release(const mk_tcp_session_ref ref);
// Get mk_tcp_session according to the strong reference
API_EXPORT mk_tcp_session mk_tcp_session_from_ref(const mk_tcp_session_ref ref);

// /////////////////////////////////////////Custom tcp service/////////////////////////////////////////////

typedef struct {
    /**
     * Receive mk_tcp_session create object
     * @param server_port Server port number
     * @param session Session processing object
     */
    void (API_CALL *on_mk_tcp_session_create)(uint16_t server_port,mk_tcp_session session);

    /**
     * Receive data sent by the client
     * @param server_port Server port number
     * @param session Session processing object
     * @param buffer Data
     */
    void (API_CALL *on_mk_tcp_session_data)(uint16_t server_port,mk_tcp_session session, mk_buffer buffer);

    /**
     * Timer every 2 seconds, used to manage timeout tasks
     * @param server_port Server port number
     * @param session Session processing object
     */
    void (API_CALL *on_mk_tcp_session_manager)(uint16_t server_port,mk_tcp_session session);

    /**
     * Generally triggered by client disconnecting tcp
     * @param server_port Server port number
     * @param session Session processing object
     * @param code Error code
     * @param msg Error message
     */
    void (API_CALL *on_mk_tcp_session_disconnect)(uint16_t server_port,mk_tcp_session session,int code,const char *msg);
} mk_tcp_session_events;


typedef enum {
    // Ordinary tcp
    mk_type_tcp = 0,
    // ssl type tcp
    mk_type_ssl = 1,
    // Websocket based connection
    mk_type_ws = 2,
    // Ssl websocket based connection
    mk_type_wss = 3
}mk_tcp_type;

/**
 * Attach user data to the tcp session object
 * This function is only valid for services started by mk_tcp_server_server_start
 * @param session Session object
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_tcp_session_set_user_data(mk_tcp_session session, void *user_data);
API_EXPORT void API_CALL mk_tcp_session_set_user_data2(mk_tcp_session session, void *user_data, on_user_data_free user_data_free);

/**
 * Get the user data attached to the tcp session object
 * This function is only valid for services started by mk_tcp_server_server_start
 * @param session Tcp session object
 * @return User data pointer
 */
API_EXPORT void* API_CALL mk_tcp_session_get_user_data(mk_tcp_session session);

/**
 * Start tcp server
 * @param port Listening port number, 0 is random
 * @param type Server type
 */
API_EXPORT uint16_t API_CALL mk_tcp_server_start(uint16_t port, mk_tcp_type type);

/**
 * Listen for tcp server events
 */
API_EXPORT void API_CALL mk_tcp_server_events_listen(const mk_tcp_session_events *events);


// /////////////////////////////////////////Custom tcp client/////////////////////////////////////////////

typedef struct mk_tcp_client_t *mk_tcp_client;
// Get the base class pointer to get its network information
API_EXPORT mk_sock_info API_CALL mk_tcp_client_get_sock_info(const mk_tcp_client ctx);

typedef struct {
    /**
     * Tcp client connects to server successfully or fails callback
     * @param client Tcp client
     * @param code 0 for successful connection, otherwise for failure reason
     * @param msg Connection failure error message
     */
    void (API_CALL *on_mk_tcp_client_connect)(mk_tcp_client client,int code,const char *msg);

    /**
     * Tcp client disconnects from tcp server callback
     * Generally caused by eof event
     * @param client Tcp client
     * @param code Error code
     * @param msg Error message
     */
    void (API_CALL *on_mk_tcp_client_disconnect)(mk_tcp_client client,int code,const char *msg);

    /**
     * Receive data sent by the tcp server
     * @param client Tcp client
     * @param buffer Data
     */
    void (API_CALL *on_mk_tcp_client_data)(mk_tcp_client client, mk_buffer buffer);

    /**
     * Timer every 2 seconds, used to manage timeout tasks
     * @param client Tcp client
     */
    void (API_CALL *on_mk_tcp_client_manager)(mk_tcp_client client);
} mk_tcp_client_events;

/**
 * Create tcp client
 * @param events Callback function structure
 * @param user_data User data pointer
 * @param type Client type
 * @return Client object
 */
API_EXPORT mk_tcp_client API_CALL mk_tcp_client_create(mk_tcp_client_events *events, mk_tcp_type type);

/**
 * Release the tcp client
 * @param ctx Client object
 */
API_EXPORT void API_CALL mk_tcp_client_release(mk_tcp_client ctx);

/**
 * Initiate connection
 * @param ctx Client object
 * @param host Server ip or domain name
 * @param port Server port number
 * @param time_out_sec Timeout time
 */
API_EXPORT void API_CALL mk_tcp_client_connect(mk_tcp_client ctx, const char *host, uint16_t port, float time_out_sec);

/**
 * Non-thread-safe data sending
 * Developers can call this function if they can ensure that it is within the network thread of this object
 * @param ctx Client object
 * @param data Data pointer
 * @param len Data length, 0 means get it by strlen internally
 */
API_EXPORT void API_CALL mk_tcp_client_send(mk_tcp_client ctx, const char *data, int len);
API_EXPORT void API_CALL mk_tcp_client_send_buffer(mk_tcp_client ctx, mk_buffer buffer);

/**
 * Send data after switching to the network thread of this object
 * @param ctx Client object
 * @param data Data pointer
 * @param len Data length, 0 means get it by strlen internally
 */
API_EXPORT void API_CALL mk_tcp_client_send_safe(mk_tcp_client ctx, const char *data, int len);
API_EXPORT void API_CALL mk_tcp_client_send_buffer_safe(mk_tcp_client ctx, mk_buffer buffer);

/**
 * Client attaches user data
 * @param ctx Client object
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_tcp_client_set_user_data(mk_tcp_client ctx, void *user_data);
API_EXPORT void API_CALL mk_tcp_client_set_user_data2(mk_tcp_client ctx, void *user_data, on_user_data_free user_data_free);

/**
 * Get the user data attached to the client object
 * @param ctx Client object
 * @return User data pointer
 */
API_EXPORT void* API_CALL mk_tcp_client_get_user_data(mk_tcp_client ctx);

#ifdef __cplusplus
}
#endif
#endif //MK_TCP_H
