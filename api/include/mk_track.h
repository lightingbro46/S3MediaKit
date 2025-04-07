#ifndef ZLMEDIAKIT_MK_TRACK_H
#define ZLMEDIAKIT_MK_TRACK_H

#include "mk_common.h"
#include "mk_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// Audio and video track
typedef struct mk_track_t *mk_track;
// Output frame callback
typedef void(API_CALL *on_mk_frame_out)(void *user_data, mk_frame frame);

// Track creation parameters
typedef union {
    struct {
        int width;
        int height;
        int fps;
    } video;

    struct {
        int channels;
        int sample_rate;
    } audio;
} codec_args;

/**
 * Create a track object reference
 * @param codec_id Please refer to the MKCodecXXX constant definition
 * @param args Video or audio parameters
 * @return Track object reference
 */
API_EXPORT mk_track API_CALL mk_track_create(int codec_id, codec_args *args);

/**
 * Decrement the reference count of the track object
 * @param track Track object
 */
API_EXPORT void API_CALL mk_track_unref(mk_track track);

/**
 * Increment the reference count of the track object
 * @param track Track object
 * @return New track reference object
 */
API_EXPORT mk_track API_CALL mk_track_ref(mk_track track);

/**
 * Get the track encoding codec type, please refer to the MKCodecXXX definition
 */
API_EXPORT int API_CALL mk_track_codec_id(mk_track track);

/**
 * Get the encoding codec name
 */
API_EXPORT const char* API_CALL mk_track_codec_name(mk_track track);

/**
 * Get the bitrate information
 */
API_EXPORT int API_CALL mk_track_bit_rate(mk_track track);

/**
 * Get whether the track is ready, 1: ready, 0: not ready
 */
API_EXPORT int API_CALL mk_track_ready(mk_track track);

/**
 * Get the cumulative frame count
 */
API_EXPORT uint64_t API_CALL mk_track_frames(mk_track track);

/**
 * Get the time, in milliseconds
 */
API_EXPORT uint64_t API_CALL mk_track_duration(mk_track track);

/**
 * Listen for frame output events
 * @param track Track object
 * @param cb Frame output callback
 * @param user_data Frame output callback user pointer parameter
 */
API_EXPORT void *API_CALL mk_track_add_delegate(mk_track track, on_mk_frame_out cb, void *user_data);
API_EXPORT void *API_CALL mk_track_add_delegate2(mk_track track, on_mk_frame_out cb, void *user_data, on_user_data_free user_data_free);

/**
 * Cancel the frame output event listener
 * @param track Track object
 * @param tag Return value of mk_track_add_delegate
 */
API_EXPORT void API_CALL mk_track_del_delegate(mk_track track, void *tag);

/**
 * Input frame to track, you usually don't need to call this api
 */
API_EXPORT void API_CALL mk_track_input_frame(mk_track track, mk_frame frame);

/**
 * Whether the track is video
 */
API_EXPORT int API_CALL mk_track_is_video(mk_track track);

/**
 * Get the video width
 */
API_EXPORT int API_CALL mk_track_video_width(mk_track track);

/**
 * Get the video height
 */
API_EXPORT int API_CALL mk_track_video_height(mk_track track);

/**
 * Get the video frame rate
 */
API_EXPORT int API_CALL mk_track_video_fps(mk_track track);

/**
 * Get the cumulative number of video keyframes
 */
API_EXPORT uint64_t API_CALL mk_track_video_key_frames(mk_track track);

/**
 * Get the video GOP keyframe interval
 */
API_EXPORT int API_CALL mk_track_video_gop_size(mk_track track);

/**
 * Get the cumulative video keyframe interval (milliseconds)
 */
API_EXPORT int API_CALL mk_track_video_gop_interval_ms(mk_track track);

/**
 * Get the audio sample rate
 */
API_EXPORT int API_CALL mk_track_audio_sample_rate(mk_track track);

/**
 * Get the number of audio channels
 */
API_EXPORT int API_CALL mk_track_audio_channel(mk_track track);

/**
 * Get the audio bit depth, usually 16bit
 */
API_EXPORT int API_CALL mk_track_audio_sample_bit(mk_track track);

#ifdef __cplusplus
}
#endif

#endif //ZLMEDIAKIT_MK_TRACK_H