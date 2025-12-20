#ifndef MK_WEBRTC_H
#define MK_WEBRTC_H
#include "mk_common.h"
#include "mk_proxyplayer.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


// Get webrtc answer sdp callback function
typedef void(API_CALL *on_mk_webrtc_get_answer_sdp)(void *user_data, const char *answer, const char *err);

// Get webrtc proxy player info callback function
typedef void(API_CALL *on_mk_webrtc_get_proxy_player_info_cb)(const char *info_json, const char *err);

// WebRTC - Register to signaling server, WebRTC - Unregister from signaling server callback function
typedef void(API_CALL *on_mk_webrtc_room_keeper_info_cb)(void *user_data, const char *room_key, const char *err);

// Get WebRTC - Peer view registration information, WebRTC - Signaling server view registration information callback function
typedef void(API_CALL *on_mk_webrtc_room_keeper_data_cb)(const char *data);


/**
 * webrtc exchange sdp, generate answer sdp based on offer sdp
 * @param user_data Callback user pointer
 * @param cb Callback function
 * @param type webrtc plugin type, supports echo, play, push
 * @param offer webrtc offer sdp
 * @param url rtc url, for example rtc://__defaultVhost/app/stream?key1=val1&key2=val2
 */
API_EXPORT void API_CALL mk_webrtc_get_answer_sdp(void *user_data, on_mk_webrtc_get_answer_sdp cb, const char *type, const char *offer, const char *url);

API_EXPORT void API_CALL mk_webrtc_get_answer_sdp2(
    void *user_data, on_user_data_free user_data_free, on_mk_webrtc_get_answer_sdp cb, const char *type, const char *offer, const char *url);

/**
 * Get webrtc proxy player information
 * @param mk_proxy_player proxy
 * @param cb callback function
 */
API_EXPORT void API_CALL mk_webrtc_get_proxy_player_info(mk_proxy_player ctx, on_mk_webrtc_get_proxy_player_info_cb cb);


/**
 * WebRTC -Register to signaling server
 * @param server_host signaling server host
 * @param server_port signaling server port
 * @param room_id room id
 * @param ssl enable ssl
 * @param cb callback function
 * @param user_data user data
 */
API_EXPORT void API_CALL
mk_webrtc_add_room_keeper(const char *room_id, const char *server_host, uint16_t server_port, int ssl, on_mk_webrtc_room_keeper_info_cb cb, void *user_data);


API_EXPORT void API_CALL mk_webrtc_add_room_keeper2(
    const char *room_id, const char *server_host, uint16_t server_port, int ssl, on_mk_webrtc_room_keeper_info_cb cb, void *user_data,
    on_user_data_free user_data_free);


/**
 * WebRTC - Unregister from signaling server
 * @param room_key room key
 * @param cb callback function
 * @param user_data user data
 */
API_EXPORT void API_CALL mk_webrtc_del_room_keeper(const char *room_key, on_mk_webrtc_room_keeper_info_cb cb, void *user_data);

API_EXPORT void API_CALL
mk_webrtc_del_room_keeper2(const char *room_key, on_mk_webrtc_room_keeper_info_cb cb, void *user_data, on_user_data_free user_data_free);


/**
 * WebRTC - Peer view registration information
 * @param cb callback function
 */
API_EXPORT void API_CALL mk_webrtc_list_room_keeper(on_mk_webrtc_room_keeper_data_cb cb);

/**
 * WebRTC - Signaling server view registration information
 * @param cb callback function
 */
API_EXPORT void API_CALL mk_webrtc_list_rooms(on_mk_webrtc_room_keeper_data_cb cb);

#ifdef __cplusplus
}
#endif

#endif /* MK_WEBRTC_H */