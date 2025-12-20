#ifndef MK_RECORDER_API_H_
#define MK_RECORDER_API_H_

#include "mk_common.h"
#include "mk_util.h"

#ifdef __cplusplus
extern "C" {
#endif

// /////////////////////////////////////////FLV recording/////////////////////////////////////////////

typedef struct mk_flv_recorder_t *mk_flv_recorder;

/**
 * Create flv recorder
 * @return
 */
API_EXPORT mk_flv_recorder API_CALL mk_flv_recorder_create();

/**
 * Release flv recorder
 * @param ctx
 */
API_EXPORT void API_CALL mk_flv_recorder_release(mk_flv_recorder ctx);

/**
 * Start recording flv
 * @param ctx flv recorder
 * @param vhost virtual host
 * @param app app name of the bound RtmpMediaSource
 * @param stream stream name of the bound RtmpMediaSource
 * @param file_path file storage address
 * @return 0: start exceeds, -1: failure, file opening fails or the RtmpMediaSource does not exist
 */
API_EXPORT int API_CALL mk_flv_recorder_start(mk_flv_recorder ctx, const char *vhost, const char *app, const char *stream, const char *file_path);

// /////////////////////////////////////////hls/mp4 recording/////////////////////////////////////////////

/**
 * Get recording status
 * @param type 0: hls, 1: MP4
 * @param vhost virtual host
 * @param app application name
 * @param stream stream id
 * @return recording status, 0: not recording, 1: recording
 */
API_EXPORT int API_CALL mk_recorder_is_recording(int type, const char *vhost, const char *app, const char *stream);

/**
 * Start recording
 * @param type 0: hls-ts, 1: MP4, 2: hls-fmp4, 3: http-fmp4, 4: http-ts
 * @param vhost virtual host
 * @param app application name
 * @param stream stream id
 * @param customized_path custom directory for saving recording files, defaults to empty or null, automatically generated
 * @param max_second maximum slice time for mp4 recording, in seconds, set to 0 to use the configuration file configuration
 * @return 1 represents success, 0 represents failure
 */
API_EXPORT int API_CALL mk_recorder_start(int type, const char *vhost, const char *app, const char *stream, const char *customized_path, size_t max_second);

/**
 * Stop recording
 * @param type 0: hls-ts, 1: MP4, 2: hls-fmp4, 3: http-fmp4, 4: http-ts
 * @param vhost virtual host
 * @param app application name
 * @param stream stream id
 * @return 1: success, 0: failure
 */
API_EXPORT int API_CALL mk_recorder_stop(int type, const char *vhost, const char *app, const char *stream);

/**
 * Start event video recording
 * @param vhost virtual host
 * @param app application name
 * @param stream stream id
 * @param path relative path to save the video file, including name
 * @param back_ms Backtracking recording duration
 * @param forward_ms subsequent recording duration
 * @return 1: success, 0: failure
 * */
API_EXPORT int API_CALL mk_recorder_start_task(const char *vhost, const char *app, const char *stream, const char *path, uint32_t back_ms, uint32_t forward_ms);


/**
 * Load mp4 list
 * @param vhost Virtual Host
 * @param app Application name
 * @param stream Stream id
 * @param file_path File path
 * @param file_repeat Cyclic demultiplexing
 * @param ini Configuration
 */
API_EXPORT void API_CALL mk_load_mp4_file(const char *vhost, const char *app, const char *stream, const char *file_path, int file_repeat);
API_EXPORT void API_CALL mk_load_mp4_file2(const char *vhost, const char *app, const char *stream, const char *file_path, int file_repeat, mk_ini ini);

#ifdef __cplusplus
}
#endif

#endif /* MK_RECORDER_API_H_ */
