#ifndef ZLMEDIAKIT_MK_FRAME_H
#define ZLMEDIAKIT_MK_FRAME_H

#include "mk_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// cpp
// Is it a keyframe
#define MK_FRAME_FLAG_IS_KEY (1 << 0)
// Is it a configuration frame?(wait sps/pps/vps)
#define MK_FRAME_FLAG_IS_CONFIG (1 << 1)
// Whether a discardable frame is(sei/aud)
#define MK_FRAME_FLAG_DROP_ABLE  (1 << 2)
// Whether frames that cannot be decoded separately(Multi-slice non-vcl frames)
#define MK_FRAME_FLAG_NOT_DECODE_ABLE (1 << 3)

// codec id constant definition
API_EXPORT extern const int MKCodecH264;
API_EXPORT extern const int MKCodecH265;
API_EXPORT extern const int MKCodecAAC;
API_EXPORT extern const int MKCodecG711A;
API_EXPORT extern const int MKCodecG711U;
API_EXPORT extern const int MKCodecOpus;
API_EXPORT extern const int MKCodecL16;
API_EXPORT extern const int MKCodecVP8;
API_EXPORT extern const int MKCodecVP9;
API_EXPORT extern const int MKCodecAV1;
API_EXPORT extern const int MKCodecJPEG;

typedef struct mk_frame_t *mk_frame;

// User-defined free callback function
typedef void(API_CALL *on_mk_frame_data_release)(void *user_data, char *ptr);

/**
 * Create a frame object and return its reference.
 * @param codec_id Encoding and decoding type, please refer to MKCodecXXX definition.
 * @param dts Decoding timestamp, in milliseconds.
 * @param pts Display timestamp, in milliseconds.
 * @param data Single frame data.
 * @param size Single frame data length.
 * @param cb data pointer free release callback, if empty, the data will be copied internally.
 * @param user_data data pointer free release callback user pointer.
 * @return frame object reference.
 */
API_EXPORT mk_frame API_CALL mk_frame_create(int codec_id, uint64_t dts, uint64_t pts, const char *data, size_t size,
                                            on_mk_frame_data_release cb, void *user_data);
API_EXPORT mk_frame API_CALL mk_frame_create2(int codec_id, uint64_t dts, uint64_t pts, const char *data, size_t size,
                                             on_mk_frame_data_release cb, void *user_data, on_user_data_free user_data_free);
/**
 * Decrement the reference of the frame object.
 * @param frame Frame object reference.
 */
API_EXPORT void API_CALL mk_frame_unref(mk_frame frame);

/**
 * Reference the frame object.
 * @param frame The referenced frame object.
 * @return New object reference.
 */
API_EXPORT mk_frame API_CALL mk_frame_ref(mk_frame frame);

/**
 * Get the frame encoding codec type, please refer to MKCodecXXX definition.
 */
API_EXPORT int API_CALL mk_frame_codec_id(mk_frame frame);

/**
 * Get the frame encoding codec name.
 */
API_EXPORT const char* API_CALL mk_frame_codec_name(mk_frame frame);

/**
 * Whether the frame is video.
 */
API_EXPORT int API_CALL mk_frame_is_video(mk_frame frame);

/**
 * Get the frame data pointer.
 */
API_EXPORT const char* API_CALL mk_frame_get_data(mk_frame frame);

/**
 * Get the length of the frame data pointer.
 */
API_EXPORT size_t API_CALL mk_frame_get_data_size(mk_frame frame);

/**
 * Return the length of the frame data prefix, for example, the H264/H265 prefix is generally 0x00 00 00 01, then this function returns 4.
 */
API_EXPORT size_t API_CALL mk_frame_get_data_prefix_size(mk_frame frame);

/**
 * Get the decoding timestamp, in milliseconds.
 */
API_EXPORT uint64_t API_CALL mk_frame_get_dts(mk_frame frame);

/**
 * Get the display timestamp, in milliseconds.
 */
API_EXPORT uint64_t API_CALL mk_frame_get_pts(mk_frame frame);

/**
 * Get the frame flag, please refer to MK_FRAME_FLAG.
 */
API_EXPORT uint32_t API_CALL mk_frame_get_flags(mk_frame frame);

//////////////////////////////////////////////////////////////////////

typedef struct mk_buffer_t *mk_buffer;
typedef struct mk_frame_merger_t *mk_frame_merger;

/**
 * Create a frame merger.
 * @param type Starting header type, 0: none, 1: h264_prefix/AnnexB(0x 00 00 00 01), 2: mp4_nal_size(avcC)
 * @return Frame merger.
 */
API_EXPORT mk_frame_merger API_CALL mk_frame_merger_create(int type);

/**
 * Destroy the frame merger.
 * @param ctx Object pointer.
 */
API_EXPORT void API_CALL mk_frame_merger_release(mk_frame_merger ctx);

/**
 * Clear the merger object buffer for reuse.
 * @param ctx Object pointer.
 */
API_EXPORT void API_CALL mk_frame_merger_clear(mk_frame_merger ctx);

/**
 * Frame merging callback function.
 * @param user_data User data pointer.
 * @param dts Decoding timestamp.
 * @param pts Display timestamp.
 * @param buffer Merged data buffer object.
 * @param have_key_frame Whether the merged data contains a key frame.
 */
typedef void(API_CALL *on_mk_frame_merger)(void *user_data, uint64_t dts, uint64_t pts, mk_buffer buffer, int have_key_frame);

/**
 * Input frame to the merger object and merge.
 * @param ctx Object pointer.
 * @param frame Frame data.
 * @param cb Frame merging callback function.
 * @param user_data Frame merging callback function user data pointer.
 */
API_EXPORT void API_CALL mk_frame_merger_input(mk_frame_merger ctx, mk_frame frame, on_mk_frame_merger cb, void *user_data);

/**
 * Force flush the merger object buffer. Before calling this API, make sure to call the mk_frame_merger_input function first and the callback parameters are valid.
 * @param ctx Object pointer.
 */
API_EXPORT void API_CALL mk_frame_merger_flush(mk_frame_merger ctx);

//////////////////////////////////////////////////////////////////////

typedef struct mk_mpeg_muxer_t *mk_mpeg_muxer;

/**
 * mpeg-ps/ts packer output callback function.
 * @param user_data User data pointer set during callback.
 * @param muxer Object.
 * @param frame Frame data.
 * @param size Frame data length.
 * @param timestamp Timestamp.
 * @param key_pos Whether it is a key frame.
 */
typedef void(API_CALL *on_mk_mpeg_muxer_frame)(void *user_data, mk_mpeg_muxer muxer, const char *frame, size_t size, uint64_t timestamp, int key_pos);

/**
 * mpeg-ps/ts packer.
 * @param cb Packing callback function.
 * @param user_data Callback user data pointer.
 * @param is_ps Whether it is ps.
 * @return Packer object.
 */
API_EXPORT mk_mpeg_muxer API_CALL mk_mpeg_muxer_create(on_mk_mpeg_muxer_frame cb, void *user_data, int is_ps);

/**
 * Delete the mpeg-ps/ts packer.
 * @param ctx Packer.
 */
API_EXPORT void API_CALL mk_mpeg_muxer_release(mk_mpeg_muxer ctx);

/**
 * Add audio/video track.
 * @param ctx mk_mpeg_muxer object.
 * @param track mk_track object, audio/video track.
 */
API_EXPORT void API_CALL mk_mpeg_muxer_init_track(mk_mpeg_muxer ctx, void* track);

/**
 * Call this function after the track is initialized.
 * In the case of a single track (only audio or video), because S3MediaKit does not know whether to add more tracks later, it will wait for an additional 3 seconds.
 * If the generated stream is a single Track type, please call this function to speed up the stream generation. Of course, if you don't call this function, the impact is not big (it will wait for an additional 3 seconds).
 * @param ctx Object pointer.
 */
API_EXPORT void API_CALL mk_mpeg_muxer_init_complete(mk_mpeg_muxer ctx);

/**
 * Input frame object.
 * @param ctx mk_mpeg_muxer object.
 * @param frame Frame object.
 * @return 1 means success, 0 means failure.
 */
API_EXPORT int API_CALL mk_mpeg_muxer_input_frame(mk_mpeg_muxer ctx, mk_frame frame);

//////////////////////////////////////////////////////////////////////
#if defined(ENABLE_RTPPROXY)

typedef struct mk_ps_decoder_t *mk_ps_decoder;

typedef void (API_CALL *on_mk_ps_decoder_stream)(void *user_data, int stream, int codecid, const void *ext, size_t ext_len, int finish);
typedef void(API_CALL *on_mk_ps_decoder_frame)(void *user_data, int stream, int codecid, int flags, int64_t pts, int64_t dts, const void *data, size_t bytes);

/**
 * Create a ps parser
 * @param scb stream callback; optional, if you know the data type explicitly, this callback may not be required to create a track?
 * @param dcb Data callback; required
 * @param user_data User-defined data
 * @return
 */
API_EXPORT mk_ps_decoder API_CALL mk_ps_decoder_create(on_mk_ps_decoder_stream scb, on_mk_ps_decoder_frame dcb, void * user_data);

/**
 * Release the ps parser
 * @param ctx
 */
API_EXPORT void API_CALL mk_ps_decoder_release(mk_ps_decoder ctx);

/**
 * Enter ps data
 * @param ctx ps parser pointer
 * @param data ps data pointer
 * @param bytes Data length
 */
API_EXPORT void API_CALL mk_ps_decoder_input(mk_ps_decoder ctx, const char * data, size_t bytes);


# endif

#ifdef __cplusplus
}
#endif

#endif //ZLMEDIAKIT_MK_FRAME_H
