#ifndef S3MEDIAKIT_MK_H264_SPLITTER_H
#define S3MEDIAKIT_MK_H264_SPLITTER_H

#include "mk_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mk_h264_splitter_t *mk_h264_splitter;

/**
 * h264 frame splitter output callback function
 * @param user_data user data pointer set when setting the callback
 * @param splitter object
 * @param frame frame data
 * @param size frame data length
 */
typedef void(API_CALL *on_mk_h264_splitter_frame)(void *user_data, mk_h264_splitter splitter, const char *frame, int size);

/**
 * Create h264 frame splitter
 * @param cb frame splitting callback function
 * @param user_data callback user data pointer
 * @param is_h265 whether it is 265
 * @return frame splitter object
 */
API_EXPORT mk_h264_splitter API_CALL mk_h264_splitter_create(on_mk_h264_splitter_frame cb, void *user_data, int is_h265);
API_EXPORT mk_h264_splitter API_CALL mk_h264_splitter_create2(on_mk_h264_splitter_frame cb, void *user_data, on_user_data_free user_data_free, int is_h265);

/**
 * Delete h264 frame splitter
 * @param ctx frame splitter
 */
API_EXPORT void API_CALL mk_h264_splitter_release(mk_h264_splitter ctx);

/**
 * Input data and split frames
 * @param ctx frame splitter
 * @param data h264/h265 data
 * @param size data length
 */
API_EXPORT void API_CALL mk_h264_splitter_input_data(mk_h264_splitter ctx, const char *data, int size);

#ifdef __cplusplus
}
#endif
#endif //S3MEDIAKIT_MK_H264_SPLITTER_H
