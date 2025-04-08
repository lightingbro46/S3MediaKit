#ifndef S3MEDIAKIT_MK_TRANSCODE_H
#define S3MEDIAKIT_MK_TRANSCODE_H

#include "mk_common.h"
#include "mk_track.h"
#include "mk_tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

// cpp
// Decoder object
typedef struct mk_decoder_t *mk_decoder;
// Decoded frame
typedef struct mk_frame_pix_t *mk_frame_pix;
// SwsContext wrapper
typedef struct mk_swscale_t *mk_swscale;
// FFmpeg original decoded frame object
typedef struct AVFrame AVFrame;
// FFmpeg codec object
typedef struct AVCodecContext AVCodecContext;
// Decode output callback
typedef void(API_CALL *on_mk_decode)(void *user_data, mk_frame_pix frame);

/**
 * Create decoder
 * @param track track object
 * @param thread_num Number of decoding threads, 0 for automatic
 * @return Returns the decoder object, NULL indicates failure
 */
API_EXPORT mk_decoder API_CALL mk_decoder_create(mk_track track, int thread_num);

/**
 * Create decoder
 * @param track track object
 * @param thread_num Number of decoding threads, 0 for automatic
 * @param codec_name_list Preferred ffmpeg codec name list, ending with NULL, for example: {"libopenh264", "h264_nvdec", NULL};
 *                        The higher the priority in the array, the higher the priority; if the specified codec does not exist, or does not match the mk_track_codec_id type, the internal default codec list will be used
 * @return Returns the decoder object, NULL indicates failure
 */
API_EXPORT mk_decoder API_CALL mk_decoder_create2(mk_track track, int thread_num, const char *codec_name_list[]);

/**
 * Destroy decoder
 * @param ctx Decoder object
 * @param flush_frame Whether to wait for all frames to be decoded successfully
 */
API_EXPORT void API_CALL mk_decoder_release(mk_decoder ctx,  int flush_frame);

/**
 * Decode audio and video frames
 * @param ctx Decoder
 * @param frame Frame object
 * @param async Whether to decode asynchronously
 * @param enable_merge Whether to merge frame decoding, in some cases, it is necessary to merge slices with the same timestamp into the decoder before decoding
 */
API_EXPORT void API_CALL mk_decoder_decode(mk_decoder ctx, mk_frame frame, int async, int enable_merge);

/**
 * Set the maximum frame cache backlog limit for asynchronous decoding
 */
API_EXPORT void API_CALL mk_decoder_set_max_async_frame_size(mk_decoder ctx, size_t size);

/**
 * Set decode output callback
 * @param ctx Decoder
 * @param cb Callback function
 * @param user_data User pointer parameter of the callback function
 */
API_EXPORT void API_CALL mk_decoder_set_cb(mk_decoder ctx, on_mk_decode cb, void *user_data);
API_EXPORT void API_CALL mk_decoder_set_cb2(mk_decoder ctx, on_mk_decode cb, void *user_data, on_user_data_free user_data_free);

/**
 * Get the FFmpeg original AVCodecContext object
 * @param ctx Decoder
 */
API_EXPORT const AVCodecContext* API_CALL mk_decoder_get_context(mk_decoder ctx);

/////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Create a new reference to the mk_frame_pix decoding frame
 * @param frame Original reference
 * @return New reference
 */
API_EXPORT mk_frame_pix API_CALL mk_frame_pix_ref(mk_frame_pix frame);

/**
 * Decrease the reference of the decoding frame mk_frame_pix
 * @param frame Original reference
 */
API_EXPORT void API_CALL mk_frame_pix_unref(mk_frame_pix frame);

/**
 * Convert from FFmpeg AVFrame to mk_frame_pix
 * @param frame FFmpeg AVFrame
 * @return mk_frame_pix object
 */
API_EXPORT mk_frame_pix API_CALL mk_frame_pix_from_av_frame(AVFrame *frame);

/**
 * Create a mk_frame_pix object without memory copy
 * @param plane_data Multiple plane data, get its data pointer through mk_buffer_get_data
 * @param line_size Plane data line size
 * @param plane Number of data planes
 * @return mk_frame_pix object
 */
API_EXPORT mk_frame_pix API_CALL mk_frame_pix_from_buffer(mk_buffer plane_data[], int line_size[], int plane);

/**
 * Get the FFmpeg AVFrame object
 * @param frame Decoded frame mk_frame_pix
 * @return FFmpeg AVFrame object
 */
API_EXPORT AVFrame* API_CALL mk_frame_pix_get_av_frame(mk_frame_pix frame);

/////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Create an instance of the ffmpeg SwsContext wrapper
 * @param output AVPixelFormat type, AV_PIX_FMT_BGR24==3
 * @param width Target width, set to 0, then it is the same as the input
 * @param height Target height, set to 0, then it is the same as the input
 * @return SwsContext wrapper instance
 */
API_EXPORT mk_swscale mk_swscale_create(int output, int width, int height);

/**
 * Release the ffmpeg SwsContext wrapper instance
 * @param ctx SwsContext wrapper instance
 */
API_EXPORT void mk_swscale_release(mk_swscale ctx);

/**
 * Use SwsContext to convert pix format
 * @param ctx SwsContext wrapper instance
 * @param frame pix frame
 * @param out Data pointer to store the converted data, the user needs to ensure that the application is applied in advance and the size is sufficient
 * @return sws_scale() return value: the height of the output slice
 */
API_EXPORT int mk_swscale_input_frame(mk_swscale ctx, mk_frame_pix frame, uint8_t *out);

/**
 * Use SwsContext to convert pix format
 * @param ctx SwsContext wrapper instance
 * @param frame pix frame
 * @return New pix frame object, needs to be destroyed using mk_frame_pix_unref
 */
API_EXPORT mk_frame_pix mk_swscale_input_frame2(mk_swscale ctx, mk_frame_pix frame);

/////////////////////////////////////////////////////////////////////////////////////////////

API_EXPORT uint8_t **API_CALL mk_get_av_frame_data(AVFrame *frame);
API_EXPORT void API_CALL mk_set_av_frame_data(AVFrame *frame, uint8_t *data, int plane);

API_EXPORT int *API_CALL mk_get_av_frame_line_size(AVFrame *frame);
API_EXPORT void API_CALL mk_set_av_frame_line_size(AVFrame *frame, int line_size, int plane);

API_EXPORT int64_t API_CALL mk_get_av_frame_dts(AVFrame *frame);
API_EXPORT void API_CALL mk_set_av_frame_dts(AVFrame *frame, int64_t dts);

API_EXPORT int64_t API_CALL mk_get_av_frame_pts(AVFrame *frame);
API_EXPORT void API_CALL mk_set_av_frame_pts(AVFrame *frame, int64_t pts);

API_EXPORT int API_CALL mk_get_av_frame_width(AVFrame *frame);
API_EXPORT void API_CALL mk_set_av_frame_width(AVFrame *frame, int width);

API_EXPORT int API_CALL mk_get_av_frame_height(AVFrame *frame);
API_EXPORT void API_CALL mk_set_av_frame_height(AVFrame *frame, int height);

API_EXPORT int API_CALL mk_get_av_frame_format(AVFrame *frame);
API_EXPORT void API_CALL mk_set_av_frame_format(AVFrame *frame, int format);

#ifdef __cplusplus
}
#endif

#endif //S3MEDIAKIT_MK_TRANSCODE_H