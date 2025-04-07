#ifndef MK_PLAYER_H_
#define MK_PLAYER_H_

#include "mk_common.h"
#include "mk_frame.h"
#include "mk_track.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mk_player_t *mk_player;

/**
 * Callback for playback result or playback interruption event
 * @param user_data User data pointer
 * @param err_code Error code, 0 for success
 * @param err_msg Error message
 * @param tracks Track list
 * @param track_count Number of tracks
 */
typedef void(API_CALL *on_mk_play_event)(void *user_data, int err_code, const char *err_msg, mk_track tracks[],
                                         int track_count);

/**
 * Create a player that supports rtmp[s]/rtsp[s]
 * @return Player pointer
 */
API_EXPORT mk_player API_CALL mk_player_create();

/**
 * Destroy the player
 * @param ctx Player pointer
 */
API_EXPORT void API_CALL mk_player_release(mk_player ctx);

/**
 * Set player configuration options
 * @param ctx Player pointer
 * @param key Configuration key, supports net_adapter/rtp_type/rtsp_user/rtsp_pwd/protocol_timeout_ms/media_timeout_ms/beat_interval_ms/wait_track_ready
 * @param val Configuration value, if it is an integer, it needs to be converted to a string
 */
API_EXPORT void API_CALL mk_player_set_option(mk_player ctx, const char *key, const char *val);

/**
 * Start playing the url
 * @param ctx Player pointer
 * @param url rtsp[s]/rtmp[s] url
 */
API_EXPORT void API_CALL mk_player_play(mk_player ctx, const char *url);

/**
 * Pause or resume playback, only useful for on-demand
 * @param ctx Player pointer
 * @param pause 1: Pause playback, 0: Resume playback
 */
API_EXPORT void API_CALL mk_player_pause(mk_player ctx, int pause);

/**
 * Playback at a multiple, only useful for on-demand
 * @param ctx Player pointer
 * @param speed 0.5 1.0 2.0
 */
API_EXPORT void API_CALL mk_player_speed(mk_player ctx, float speed);

/**
 * Set the on-demand progress bar
 * @param ctx Object pointer
 * @param progress Value range is 0.0～1.0
 */
API_EXPORT void API_CALL mk_player_seekto(mk_player ctx, float progress);

/**
 * Set the on-demand progress bar
 * @param ctx Object pointer
 * @param seek_pos Value range is the increment relative to the start time, unit is seconds
 */
API_EXPORT void API_CALL mk_player_seekto_pos(mk_player ctx, int seek_pos);

/**
 * Set the player to enable playback result callback function
 * @param ctx Player pointer
 * @param cb Callback function pointer, set null to immediately cancel the callback
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_player_set_on_result(mk_player ctx, on_mk_play_event cb, void *user_data);
API_EXPORT void API_CALL mk_player_set_on_result2(mk_player ctx, on_mk_play_event cb, void *user_data, on_user_data_free user_data_free);

/**
 * Set the callback for playback being abnormally interrupted
 * @param ctx Player pointer
 * @param cb Callback function pointer, set null to immediately cancel the callback
 * @param user_data User data pointer
 ///////////////////////////Audio and video related information interfaces are only valid after the playback success callback is triggered///////////////////////////////
 */
API_EXPORT void API_CALL mk_player_set_on_shutdown(mk_player ctx, on_mk_play_event cb, void *user_data);
API_EXPORT void API_CALL mk_player_set_on_shutdown2(mk_player ctx, on_mk_play_event cb, void *user_data, on_user_data_free user_data_free);

// /////////////////////////The interface to obtain audio and video related information is only valid after the callback is triggered after the playback is successful.///////////////////////////////
// * Get the duration of the on-demand program, if it is live, return 0, otherwise return the number of seconds

/**
 * Get the on-demand program duration, if it is live, return 0, otherwise return the number of seconds
 */
API_EXPORT float API_CALL mk_player_duration(mk_player ctx);

/**
 * Get on-demand playback progress, value range 0.0~1.0
 * Get the on-demand playback progress position, value range is the increment relative to the start time, unit is seconds
 */
API_EXPORT float API_CALL mk_player_progress(mk_player ctx);

/**
 * Get the on-demand playback progress position, value range, increment relative to start time, unit seconds
 * @param ctx Object pointer
 * @param track_type 0: Video, 1: Audio
 */
API_EXPORT int API_CALL mk_player_progress_pos(mk_player ctx);

/**
 * Get packet loss rate, valid when rtsp
 * @param ctx Object pointer
 * @param track_type 0: Video, 1: Audio
 */
API_EXPORT float API_CALL mk_player_loss_rate(mk_player ctx, int track_type);

#ifdef __cplusplus
}
#endif

#endif /* MK_PLAYER_H_ */
