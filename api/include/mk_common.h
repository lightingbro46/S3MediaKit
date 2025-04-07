#ifndef MK_COMMON_H
#define MK_COMMON_H

#include <stdint.h>
#include <stddef.h>

#if defined(GENERATE_EXPORT)
#include "mk_export.h"
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#    define API_CALL __cdecl
#else
#    define API_CALL
#endif

#ifndef _WIN32
#define _strdup strdup
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#    if !defined(GENERATE_EXPORT)
#        if defined(MediaKitApi_EXPORTS)
#            define API_EXPORT __declspec(dllexport)
#        else
#            define API_EXPORT __declspec(dllimport)
#        endif
#    endif
#elif !defined(GENERATE_EXPORT)
#   define API_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// cpp
// Output log to shell
#define LOG_CONSOLE     (1 << 0)
// Output log to file
#define LOG_FILE        (1 << 1)
// Output log to callback function (mk_events::on_mk_log)
#define LOG_CALLBACK    (1 << 2)

// Downward compatibility
#define mk_env_init1 mk_env_init2

// Callback user_data callback function
typedef void(API_CALL *on_user_data_free)(void *user_data);

typedef struct {
    // Number of threads
    int thread_num;

    // Log level, supports 0~4
    int log_level;
    // Control the mask of log output, please refer to LOG_CONSOLE, LOG_FILE, LOG_CALLBACK macros
    int log_mask;
    // File log save path, the path can be non-existent (folders can be created internally), set to NULL to disable log output to file
    const char *log_file_path;
    // File log save days, set to 0 to disable log file
    int log_file_days;

    // Is the configuration file content or path
    int ini_is_path;
    // Configuration file content or path, can be NULL, if the file does not exist, then the default configuration will be exported to the file
    const char *ini;

    // Is the ssl certificate content or path
    int ssl_is_path;
    // ssl certificate content or path, can be NULL
    const char *ssl;
    // Certificate password, can be NULL
    const char *ssl_pwd;
} mk_config;

/**
 * Initialize the environment, you need to call this function before calling this library
 * @param cfg Library running related parameters
 */
API_EXPORT void API_CALL mk_env_init(const mk_config *cfg);

/**
 * Close all servers, please call this function when exiting the main function
 */
API_EXPORT void API_CALL mk_stop_all_server();

/**
 * mk_env_init version of basic type parameters, for easy calling by other languages
 * @param thread_num Number of threads
 * @param log_level Log level, supports 0~4
 * @param log_mask Log output mode mask, please refer to LOG_CONSOLE, LOG_FILE, LOG_CALLBACK macros
 * @param log_file_path File log save path, the path can be non-existent (folders can be created internally), set to NULL to disable log output to file
 * @param log_file_days File log save days, set to 0 to disable log file
 * @param ini_is_path Is the configuration file content or path
 * @param ini Configuration file content or path, can be NULL, if the file does not exist, then the default configuration will be exported to the file
 * @param ssl_is_path Is the ssl certificate content or path
 * @param ssl ssl certificate content or path, can be NULL
 * @param ssl_pwd Certificate password, can be NULL
 */
API_EXPORT void API_CALL mk_env_init2(int thread_num,
                                      int log_level,
                                      int log_mask,
                                      const char *log_file_path,
                                      int log_file_days,
                                      int ini_is_path,
                                      const char *ini,
                                      int ssl_is_path,
                                      const char *ssl,
                                      const char *ssl_pwd);

/**
 * Set the log file
 * @param file_max_size Single slice file size (MB)
 * @param file_max_count Number of slice files
*/
API_EXPORT void API_CALL mk_set_log(int file_max_size, int file_max_count);

/**
 * Set the configuration item
 * @deprecated Please use mk_ini_set_option instead
 * @param key Configuration item name
 * @param val Configuration item value
 */
API_EXPORT void API_CALL mk_set_option(const char *key, const char *val);

/**
 * Get the value of the configuration item
 * @deprecated Please use mk_ini_get_option instead
 * @param key Configuration item name
 */
API_EXPORT const char * API_CALL mk_get_option(const char *key);


/**
 * Create http[s] server
 * @param port htt listening port, recommended 80, pass in 0 to randomly allocate
 * @param ssl Whether it is an ssl type server
 * @return 0: failure, non-0: port number
 */
API_EXPORT uint16_t API_CALL mk_http_server_start(uint16_t port, int ssl);

/**
 * Create rtsp[s] server
 * @param port rtsp listening port, recommended 554, pass in 0 to randomly allocate
 * @param ssl Whether it is an ssl type server
 * @return 0: failure, non-0: port number
 */
API_EXPORT uint16_t API_CALL mk_rtsp_server_start(uint16_t port, int ssl);

/**
 * Create rtmp[s] server
 * @param port rtmp listening port, recommended 1935, pass in 0 to randomly allocate
 * @param ssl Whether it is an ssl type server
 * @return 0: failure, non-0: port number
 */
API_EXPORT uint16_t API_CALL mk_rtmp_server_start(uint16_t port, int ssl);

/**
 * Create rtp server
 * @param port rtp listening port (including udp/tcp)
 * @return 0: failure, non-0: port number
 */
API_EXPORT uint16_t API_CALL mk_rtp_server_start(uint16_t port);

/**
 * Create rtc server
 * @param port rtc listening port
 * @return 0: failure, non-0: port number
 */
API_EXPORT uint16_t API_CALL mk_rtc_server_start(uint16_t port);

// Get webrtc answer sdp callback function
typedef void(API_CALL *on_mk_webrtc_get_answer_sdp)(void *user_data, const char *answer, const char *err);

/**
 * webrtc exchange sdp, generate answer sdp based on offer sdp
 * @param user_data Callback user pointer
 * @param cb Callback function
 * @param type webrtc plugin type, supports echo, play, push
 * @param offer webrtc offer sdp
 * @param url rtc url, for example rtc://__defaultVhost/app/stream?key1=val1&key2=val2
 */
API_EXPORT void API_CALL mk_webrtc_get_answer_sdp(void *user_data, on_mk_webrtc_get_answer_sdp cb, const char *type,
                                                  const char *offer, const char *url);

API_EXPORT void API_CALL mk_webrtc_get_answer_sdp2(void *user_data, on_user_data_free user_data_free, on_mk_webrtc_get_answer_sdp cb, const char *type,
                                                  const char *offer, const char *url);

/**
 * Create srt server
 * @param port srt listening port
 * @return 0: failure, non-0: port number
 */
API_EXPORT uint16_t API_CALL mk_srt_server_start(uint16_t port);


/**
 * Create shell server
 * @param port shell listening port
 * @return 0: failure, non-0: port number
 */
API_EXPORT uint16_t API_CALL mk_shell_server_start(uint16_t port);

#ifdef __cplusplus
}
#endif


#endif /* MK_COMMON_H */
