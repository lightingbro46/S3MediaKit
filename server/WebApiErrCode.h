#ifndef S3MEDIAKIT_WEBCODE_H
#define S3MEDIAKIT_WEBCODE_H

#include <string>

namespace managerkit {

/**
 * Custom api error code map definition 
 */
#define API_ERROR_CODE_MAP(XX)                                                                       \
    /* Success and general errors */                                                                 \
    XX(CODE_SUCCESS,                        "Success",                          200,         0     ) \
    XX(CODE_OTHER_EXCEPTION,                "Internal Server Error",            500,         -1    ) \
    /* 100xxx - Authorization errors */                                                              \
    XX(CODE_UNAUTHORIZED,                   "Unauthorized",                     401,         100001) \
    XX(CODE_PERMISSION_DENIED,              "Permission denied",                403,         100002) \
    XX(CODE_NO_LIVE_VIEW_PERMISSION,        "No live view permission",          401,         100003) \
    XX(CODE_NO_PLAYBACK_PERMISSION,         "No playback permission",           401,         100004) \
    XX(CODE_NO_PTZ_CONTROL_PERMISSION,      "No PTZ control permission",        401,         100005) \
    XX(CODE_NO_READ_MSERVER_PERMISSION,     "No read media server permission",  401,         100006) \
    XX(CODE_NO_MODIFY_MSERVER_PERMISSION,   "No modify media server permission",401,         100007) \
    XX(CODE_NO_ADD_CAMERA_PERMISSION,       "No add camera permission",         401,         100008) \
    /* 200xxx - Resource errors */                                                                   \
    XX(CODE_DEVICE_NOT_FOUND,               "Device not found",                 404,         200001) \
    XX(CODE_STREAM_NOT_FOUND,               "Stream not found",                 404,         200002) \
    XX(CODE_BOOKMARK_NOT_FOUND,             "Bookmark not found",               404,         200003) \
    XX(CODE_EXTRACT_KEY_NOT_FOUND,          "Extract key not found",            404,         200004) \
    XX(CODE_BOOKMARK_ID_NOT_FOUND,          "Bookmark id not found",            404,         200005) \
    XX(CODE_TIMELINE_NOT_FOUND,             "Timeline not found",               404,         200006) \
    XX(CODE_MSERVER_NOT_FOUND,              "Media server not found",           404,         200007) \
    XX(CODE_SCAN_KEY_NOT_FOUND,             "Scan key not found",               404,         200008) \
    XX(CODE_PTZ_PRESET_NOT_FOUND,           "User preset not found",            404,         200009) \
    /* 300xxx - Operation errors */                                                                  \
    XX(CODE_DEVICE_OFFLINE,                 "Device offline",                   400,         300001) \
    XX(CODE_STREAM_OFFLINE,                 "Stream offline",                   400,         300002) \
    XX(CODE_EXTRACT_FAILED,                 "Extract video failed",             500,         300003) \
    XX(CODE_DEVICE_NO_SUPPORT_PTZ,          "Device does not support PTZ",      500,         300004) \
    XX(CODE_DEVICE_IS_RUNNING_PTZ,          "Device is running PTZ control",    500,         300005) \
    XX(CODE_PTZ_ABSOLUTED_CONTROL_FAILED,   "PTZ Absoluted control failed",     500,         300006) \
    XX(CODE_PTZ_CONTINUOUS_CONTROL_FAILED,  "PTZ Continuous control failed",    500,         300007) \
    XX(CODE_PTZ_RELATIVE_CONTROL_FAILED,    "PTZ Relative control failed",      500,         300008) \
    XX(CODE_DEVICE_CONFIG_DISABLE_PTZ,      "Device is configured to disable PTZ control", 403, 300009) \
    XX(CODE_HEALTH_CHECK_API_FAILED,        "Health check api service failed",  500,         300010) \
    XX(CODE_HEALTH_CHECK_MSERVER_FAILED,    "Health check media server service failed",  500, 300011) \
    XX(CODE_SUBNETSCAN_FAILED,              "Scan device by ip range failed",   500,         300012) \
    XX(CODE_DEVICE_NO_SUPPORT_SELECTED_PTZ_MODE, "Device does not support selected PTZ mode", 500, 300013) \
    XX(CODE_DEVICE_OWNERSHIP_BY_OTHER,      "Device is controlled by other user", 403,       300014) \
    XX(CODE_PTZ_GOTO_HOME_FAILED,           "PTZ Goto Home failed",             500,         300015) \
    XX(CODE_PTZ_GET_STATUS_FAILED,          "PTZ Get status failed",            500,         300016) \
    XX(CODE_PTZ_GOTO_PRESET_FAILED,         "PTZ Goto preset failed",           500,         300017) \
    XX(CODE_PTZ_GOTO_USER_PRESET_FAILED,    "PTZ Goto preset failed",           500,         300018) \
    XX(CODE_DEVICE_NO_SUPPORT_PTZ_PRESET,   "Device does not support PTZ Preset", 500,       300019) \
    XX(CODE_STREAM_READER_ON_MSERVER_LIMITED, "Stream reader on media server reached limit ", 429, 300020) \
    XX(CODE_STREAM_READER_PER_CAMERA_LIMITED, "Stream reader on device reached limit", 429,  300021) \
    /* 400xxx - Input errors */                                                                      \
    XX(CODE_INVALID_ARGS,                   "Invalid arguments",                400,         400001) \
    XX(CODE_INVALID_IP,                     "Invalid IP address",               400,         400002) \
    XX(CODE_INVALID_EXTENSION,              "Invalid file extension",           400,         400003) \
    XX(CODE_INVALID_IP_RANGE,               "Invalid IP range",                 400,         400004) \
    XX(CODE_FEATURE_NOT_SUPPORTED,          "Feature not supported",            400,         400005) \
    /* 500xxx - Database esc errors */                                                               \

typedef enum {
#define XX(type, message, http_code, custom_code) type = custom_code,
    API_ERROR_CODE_MAP(XX)
#undef XX

} ApiErrCode;

/**
 * Check whether the code id is valid
 */
bool isValidApiErrCode(int code);

/**
 * Get custom message by codeid
 */
const char* getDefaultMessage(ApiErrCode code = ApiErrCode::CODE_SUCCESS);

/**
 * Get http code by codeid
 */
int getStatusCode(ApiErrCode code = ApiErrCode::CODE_SUCCESS);

/**
 * Get ApiErrCode by permission code
 */
ApiErrCode getApiErrCodeWithPermission(const std::string &code);

} // namespace managerkit

#endif // S3MEDIAKIT_WEBCODE_H