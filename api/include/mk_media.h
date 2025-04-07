#ifndef MK_MEDIA_H_
#define MK_MEDIA_H_

#include "mk_common.h"
#include "mk_track.h"
#include "mk_frame.h"
#include "mk_events_objects.h"
#include "mk_thread.h"
#include "mk_util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mk_media_t *mk_media;

/**
 * Create a media source
 * @param vhost Virtual host name, generally __defaultVhost__
 * @param app Application name, recommended as live
 * @param stream Stream id, such as camera
 * @param duration Duration (in seconds), 0 for live broadcast
 * @param hls_enabled Whether to generate hls
 * @param mp4_enabled Whether to generate mp4
 * @return Object pointer
 */
API_EXPORT mk_media API_CALL mk_media_create(const char *vhost, const char *app, const char *stream,
                                             float duration, int hls_enabled, int mp4_enabled);

/**
 * Create a media source
 * @param vhost Virtual host name, generally __defaultVhost__
 * @param app Application name, recommended as live
 * @param stream Stream id, such as camera
 * @param duration Duration (in seconds), 0 for live broadcast
 * @param option ProtocolOption related configuration
 * @return Object pointer
 */
API_EXPORT mk_media API_CALL mk_media_create2(const char *vhost, const char *app, const char *stream, float duration, mk_ini option);

/**
 * Destroy the media source
 * @param ctx Object pointer
 */
API_EXPORT void API_CALL mk_media_release(mk_media ctx);

/**
 * Add audio and video tracks
 * @param ctx mk_media object
 * @param track mk_track object, audio and video track
 */
API_EXPORT void API_CALL mk_media_init_track(mk_media ctx, mk_track track);

/**
 * Add video track, please use mk_media_init_track method
 * @param ctx Object pointer
 * @param codec_id  0:CodecH264/1:CodecH265
 * @param width Video width; Valid only during encoding
 * @param height Video height; Valid only during encoding
 * @param fps Video fps; Valid only during encoding
 * @param bit_rate Video bitrate, unit bps; Valid only during encoding
 * @param width Video width
 * @param height Video height
 * @param fps Video fps
 * @return 1 for success, 0 for failure
 */
API_EXPORT int API_CALL mk_media_init_video(mk_media ctx, int codec_id, int width, int height, float fps, int bit_rate);

/**
 * Add audio track, please use mk_media_init_track method
 * @param ctx Object pointer
 * @param codec_id  2:CodecAAC/3:CodecG711A/4:CodecG711U/5:OPUS
 * @param channel Number of channels
 * @param sample_bit Sampling bit, only supports 16
 * @param sample_rate Sampling rate
 * @return 1 for success, 0 for failure
 */
API_EXPORT int API_CALL mk_media_init_audio(mk_media ctx, int codec_id, int sample_rate, int channels, int sample_bit);

/**
 * Call this function after h264/h265/aac initialization,
 * In single track (only audio or video), because S3MediaKit does not know whether to add more tracks later, it will wait for 3 seconds.
 * If the generated stream is a single Track type, please call this function to speed up the stream generation speed. Of course, if you do not call this function, the impact is not big (it will wait for 3 seconds).
 * @param ctx Object pointer
 */
API_EXPORT void API_CALL mk_media_init_complete(mk_media ctx);

/**
 * Input frame object
 * @param ctx mk_media object
 * @param frame Frame object
 * @return 1 for success, 0 for failure
 */
API_EXPORT int API_CALL mk_media_input_frame(mk_media ctx, mk_frame frame);

/**
 * Input single frame H264 video, the starting byte of the frame can be 00 00 01, 00 00 00 01, please use mk_media_input_frame method
 * @param ctx Object pointer
 * @param data Single frame H264 data
 * @param len Number of bytes of single frame H264 data
 * @param dts Decode timestamp, unit milliseconds
 * @param pts Play timestamp, unit milliseconds
 * @return 1 for success, 0 for failure
 */
API_EXPORT int API_CALL mk_media_input_h264(mk_media ctx, const void *data, int len, uint64_t dts, uint64_t pts);

/**
 * Input single frame H265 video, the starting byte of the frame can be 00 00 01, 00 00 00 01, please use mk_media_input_frame method
 * @param ctx Object pointer
 * @param data Single frame H265 data
 * @param len Number of bytes of single frame H265 data
 * @param dts Decode timestamp, unit milliseconds
 * @param pts Play timestamp, unit milliseconds
 * @return 1 for success, 0 for failure
 */
API_EXPORT int API_CALL mk_media_input_h265(mk_media ctx, const void *data, int len, uint64_t dts, uint64_t pts);

/**
 * Input YUV video data
 * @param ctx Object pointer
 * @param yuv yuv420p data
 * @param linesize yuv420p linesize
 * @param cts Video capture timestamp, unit milliseconds
 */
API_EXPORT void API_CALL mk_media_input_yuv(mk_media ctx, const char *yuv[3], int linesize[3], uint64_t cts);

/**
 * Input single frame AAC audio (specify adts header separately), please use mk_media_input_frame method
 * @param ctx Object pointer
 * @param data Single frame AAC data without adts header, adts header 7 bytes
 * @param len Number of bytes of single frame AAC data
 * @param dts Timestamp, milliseconds
 * @param adts adts header, can be null
 * @return 1 for success, 0 for failure
 */
API_EXPORT int API_CALL mk_media_input_aac(mk_media ctx, const void *data, int len, uint64_t dts, void *adts);

/**
 * Input single frame PCM audio, this function is valid only when ENABLE_FAAC is compiled
 * @param ctx Object pointer
 * @param data Single frame PCM data
 * @param len Number of bytes of single frame PCM data
 * @param dts Timestamp, milliseconds
 * @return 1 for success, 0 for failure
 */
API_EXPORT int API_CALL mk_media_input_pcm(mk_media ctx, void *data, int len, uint64_t pts);

/**
 * Input single frame OPUS/G711 audio frame, please use mk_media_input_frame method
 * @param ctx Object pointer
 * @param data Single frame audio data
 * @param len  Number of bytes of single frame audio data
 * @param dts Timestamp, milliseconds
 * @return 1 for success, 0 for failure
 */
API_EXPORT int API_CALL mk_media_input_audio(mk_media ctx, const void* data, int len, uint64_t dts);

/**
 * MediaSource.close() callback event
 * When you choose to close an associated MediaSource, it will eventually trigger this callback
 * You should call mk_media_release function and release other resources through this event
 * If you do not call mk_media_release function, then the MediaSource.close() operation will be invalid
 * @param user_data User data pointer, set by mk_media_set_on_close function
 */
typedef void(API_CALL *on_mk_media_close)(void *user_data);

/**
 * Listen to MediaSource.close() event
 * When you choose to close an associated MediaSource, it will eventually trigger this callback
 * You should call mk_media_release function and release other resources through this event
 * @param ctx Object pointer
 * @param cb Callback pointer
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_media_set_on_close(mk_media ctx, on_mk_media_close cb, void *user_data);
API_EXPORT void API_CALL mk_media_set_on_close2(mk_media ctx, on_mk_media_close cb, void *user_data, on_user_data_free user_data_free);

/**
 * Triggered when the client receives a seek request
 * @param user_data User data pointer, set by mk_media_set_on_seek
 * @param stamp_ms Seek to the timeline position, unit milliseconds
 * @return 1 means the seek request will be processed, 0 means the request will be ignored
 */
typedef int(API_CALL *on_mk_media_seek)(void *user_data,uint32_t stamp_ms);

/**
 * Triggered when the client receives a pause or resume request
 * @param user_data User data pointer, set by mk_media_set_on_pause
 * @param pause 1: pause, 0: resume
 */
typedef int(API_CALL* on_mk_media_pause)(void* user_data, int pause);

/**
 * Triggered when the client receives a speed request
 * @param user_data User data pointer, set by mk_media_set_on_pause
 * @param speed 0.5 1.0 2.0
 */
typedef int(API_CALL* on_mk_media_speed)(void* user_data, float speed);

/**
 * Listen to player seek request event
 * @param ctx Object pointer
 * @param cb Callback pointer
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_media_set_on_seek(mk_media ctx, on_mk_media_seek cb, void *user_data);
API_EXPORT void API_CALL mk_media_set_on_seek2(mk_media ctx, on_mk_media_seek cb, void *user_data, on_user_data_free user_data_free);

/**
 * Listen to player pause request event
 * @param ctx Object pointer
 * @param cb Callback pointer
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_media_set_on_pause(mk_media ctx, on_mk_media_pause cb, void *user_data);
API_EXPORT void API_CALL mk_media_set_on_pause2(mk_media ctx, on_mk_media_pause cb, void *user_data, on_user_data_free user_data_free);

/**
 * Listen to player pause request event
 * @param ctx Object pointer
 * @param cb Callback pointer
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_media_set_on_speed(mk_media ctx, on_mk_media_speed cb, void *user_data);
API_EXPORT void API_CALL mk_media_set_on_speed2(mk_media ctx, on_mk_media_speed cb, void *user_data, on_user_data_free user_data_free);

/**
 * Get the total number of viewers
 * @param ctx Object pointer
 * @return Number of viewers
 */
API_EXPORT int API_CALL mk_media_total_reader_count(mk_media ctx);

/**
 * MediaSource registration or deregistration event
 * @param user_data User data pointer set when setting the callback
 * @param sender Generated MediaSource object
 * @param regist 1 for registration event, 0 for deregistration event
 */
typedef void(API_CALL *on_mk_media_source_regist)(void *user_data, mk_media_source sender, int regist);

/**
 * Set MediaSource registration or deregistration event callback function
 * @param ctx Object pointer
 * @param cb Callback pointer
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_media_set_on_regist(mk_media ctx, on_mk_media_source_regist cb, void *user_data);
API_EXPORT void API_CALL mk_media_set_on_regist2(mk_media ctx, on_mk_media_source_regist cb, void *user_data, on_user_data_free user_data_free);

/**
 * Callback for whether rtp streaming is successful or not (after the first success, it will retry continuously)
 */
typedef on_mk_media_source_send_rtp_result on_mk_media_send_rtp_result;

/**
 * Start sending a ps-rtp stream (distinguished by ssrc), this api is thread-safe
 * @param ctx Object pointer
 * @param dst_url Target ip or domain name
 * @param dst_port Target port
 * @param ssrc rtp's ssrc, 10-base string print
 * @param con_type 0: tcp active, 1: udp active, 2: tcp passive, 3: udp passive
 * @param options Options
 * @param cb Start success or failure callback
 * @param user_data Callback user pointer
 */
API_EXPORT void API_CALL mk_media_start_send_rtp(mk_media ctx, const char *dst_url, uint16_t dst_port, const char *ssrc, int con_type, on_mk_media_send_rtp_result cb, void *user_data);
API_EXPORT void API_CALL mk_media_start_send_rtp2(mk_media ctx, const char *dst_url, uint16_t dst_port, const char *ssrc, int con_type, on_mk_media_send_rtp_result cb, void *user_data, on_user_data_free user_data_free);
API_EXPORT void API_CALL mk_media_start_send_rtp3(mk_media ctx, const char *dst_url, uint16_t dst_port, const char *ssrc, int con_type, mk_ini options, on_mk_media_send_rtp_result cb, void *user_data);
API_EXPORT void API_CALL mk_media_start_send_rtp4(mk_media ctx, const char *dst_url, uint16_t dst_port, const char *ssrc, int con_type, mk_ini options, on_mk_media_send_rtp_result cb, void *user_data,on_user_data_free user_data_free);
/**
 * Stop a certain route or all ps-rtp sending, this api is thread-safe
 * @param ctx Object pointer
 * @param ssrc rtp's ssrc, 10-base string print, if it is null or empty string, stop all rtp streaming
 */
API_EXPORT void API_CALL mk_media_stop_send_rtp(mk_media ctx, const char *ssrc);

/**
 * Get the belonging thread
 * @param ctx Object pointer
 */
API_EXPORT mk_thread API_CALL mk_media_get_owner_thread(mk_media ctx);


#ifdef __cplusplus
}
#endif

#endif /* MK_MEDIA_H_ */
