#ifndef MK_UTIL_H
#define MK_UTIL_H

#include <stdlib.h>
#include "mk_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Release resources allocated by mk api internally
 */
API_EXPORT void API_CALL mk_free(void *ptr);

/**
 * Get the path of the executable file of this program
 * @return File path, need to be mk_free after use
 */
API_EXPORT char* API_CALL mk_util_get_exe_path();

/**
 * Get the absolute path of the file in the same directory as the executable file of this program
 * @param relative_path The path of the file in the same directory, can be null
 * @return File path, need to be mk_free after use
 */
API_EXPORT char* API_CALL mk_util_get_exe_dir(const char *relative_path);

/**
 * Get the Unix standard system timestamp
 * @return Current system timestamp
 */
API_EXPORT uint64_t API_CALL mk_util_get_current_millisecond();

/**
 * Get the time string
 * @param fmt Time format, such as %Y-%m-%d %H:%M:%S
 * @return Time string, need to be mk_free after use
 */
API_EXPORT char* API_CALL mk_util_get_current_time_string(const char *fmt);

/**
 * Print binary data as string
 * @param buf Binary data
 * @param len Data length
 * @return Printable debug information, need to be mk_free after use
 */
API_EXPORT char* API_CALL mk_util_hex_dump(const void *buf, int len);

///////////////////////////////////////////mk ini/////////////////////////////////////////////
typedef struct mk_ini_t *mk_ini;

/**
 * Create ini configuration object
 */
API_EXPORT mk_ini API_CALL mk_ini_create();

/**
 * Return the global default ini configuration
 * @return Global default ini configuration, do not use mk_ini_release to release it
 */
API_EXPORT mk_ini API_CALL mk_ini_default();

/**
 * Load ini configuration file content
 * @param ini Ini object
 * @param str Configuration file content
 */
API_EXPORT void API_CALL mk_ini_load_string(mk_ini ini, const char *str);

/**
 * Load ini configuration file
 * @param ini Ini object
 * @param file Configuration file path
 */
API_EXPORT void API_CALL mk_ini_load_file(mk_ini ini, const char *file);

/**
 * Destroy ini configuration object
 */
API_EXPORT void API_CALL mk_ini_release(mk_ini ini);

/**
 * Add or overwrite configuration item
 * @param ini Configuration object
 * @param key Configuration name, two-part, such as: field.key
 * @param value Configuration value
 */
API_EXPORT void API_CALL mk_ini_set_option(mk_ini ini, const char *key, const char *value);
API_EXPORT void API_CALL mk_ini_set_option_int(mk_ini ini, const char *key, int value);

/**
 * Get configuration item
 * @param ini Configuration object
 * @param key Configuration name, two-part, such as: field.key
 * @return NULL if the configuration does not exist, otherwise return the configuration value
 */
API_EXPORT const char *API_CALL mk_ini_get_option(mk_ini ini, const char *key);

/**
 * Delete configuration item
 * @param ini Configuration object
 * @param key Configuration name, two-part, such as: field.key
 * @return 1: Success, 0: The configuration does not exist
 */
API_EXPORT int API_CALL mk_ini_del_option(mk_ini ini, const char *key);

/**
 * Export to configuration file content
 * @param ini Configuration object
 * @return Configuration file content string, need to be mk_free after use
 */
API_EXPORT char *API_CALL mk_ini_dump_string(mk_ini ini);

/**
 * Export configuration file to file
 * @param ini Configuration object
 * @param file Configuration file path
 */
API_EXPORT void API_CALL mk_ini_dump_file(mk_ini ini, const char *file);
// /////////////////////////////////////////statistics/////////////////////////////////////////////

typedef void(API_CALL *on_mk_get_statistic_cb)(void *user_data, mk_ini ini);

/**
 * Get memory data statistics
 * @param ini Store statistical results
 */
API_EXPORT void API_CALL mk_get_statistic(on_mk_get_statistic_cb cb, void *user_data, on_user_data_free free_cb);

// /////////////////////////////////////////log/////////////////////////////////////////////

/**
 * Print log
 * @param level Log level, support 0~4
 * @param file __FILE__
 * @param function __FUNCTION__
 * @param line __LINE__
 * @param fmt printf type format control string
 * @param ... Variable length parameters
 */
API_EXPORT void API_CALL mk_log_printf(int level, const char *file, const char *function, int line, const char *fmt, ...);

// The following macros can replace printf
#define log_printf(lev, ...) mk_log_printf(lev, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define log_trace(...) log_printf(0, ##__VA_ARGS__)
#define log_debug(...) log_printf(1, ##__VA_ARGS__)
#define log_info(...) log_printf(2, ##__VA_ARGS__)
#define log_warn(...) log_printf(3, ##__VA_ARGS__)
#define log_error(...) log_printf(4, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif //MK_UTIL_H
