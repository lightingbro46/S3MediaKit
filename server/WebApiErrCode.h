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
    /* 901xxx - Permission errors */                                                                 \
    XX(CODE_UNAUTHORIZED,                   "Unauthorized",                     401,         901001) \
    XX(CODE_PERMISSION_DENIED,              "Permission denied",                403,         901002) \
    XX(CODE_NO_LIVE_VIEW_PERMISSION,        "No live view permission",          401,         901003) \
    XX(CODE_NO_PLAYBACK_PERMISSION,         "No playback permission",           401,         901004) \
    XX(CODE_NO_PTZ_CONTROL_PERMISSION,      "No PTZ control permission",        401,         901005) \
    XX(CODE_NO_READ_MSERVER_PERMISSION,     "No read media server permission",  401,         901006) \
    XX(CODE_NO_MODIFY_MSERVER_PERMISSION,   "No modify media server permission",401,         901007) \
    XX(CODE_NO_ADD_CAMERA_PERMISSION,       "No add camera permission",         401,         901008) \
    XX(CODE_NO_EXTRACT_PERMISSION,          "No extract permission",            401,         901009) \
    XX(CODE_NO_BOOKMARK_PERMISSION,         "No bookmark permission",           401,         901010) \
    /* 902xxx - Input/validation errors */                                                           \
    XX(CODE_INVALID_ARGS,                   "Invalid arguments",                400,         902001) \
    XX(CODE_INVALID_IP,                     "Invalid IP address",               400,         902002) \
    XX(CODE_INVALID_EXTENSION,              "Invalid file extension",           400,         902003) \
    XX(CODE_INVALID_IP_RANGE,               "Invalid IP range",                 400,         902004) \
    XX(CODE_FEATURE_NOT_SUPPORTED,          "Feature not supported",            400,         902005) \
    XX(CODE_MSERVER_NOT_FOUND,              "Media server not found",           404,         902006) \
    XX(CODE_INVALID_SYNC_CURSOR,            "Invalid sync db cursors",          404,         902007) \
    /* 903xxx - Timeline errors */                                                                   \
    XX(CODE_TIMELINE_NOT_FOUND,             "Timeline not found",               404,         903001) \
    /* 904xxx - Thumbnail/snapshot errors */                                                         \
    XX(CODE_EXTRACT_THUMBNAIL_EMPTY,                 "Snapshot is empty",                404,         904001) \
    /* 905xxx - Device errors */                                                                     \
    XX(CODE_DEVICE_NOT_FOUND,               "Device not found",                 404,         905001) \
    XX(CODE_DEVICE_OFFLINE,                 "Device offline",                   400,         905002) \
    XX(CODE_DEVICE_OWNERSHIP_BY_OTHER,      "Device is controlled by other user", 403,       905003) \
    /* 906xxx - Stream/media server errors */                                                        \
    XX(CODE_STREAM_NOT_FOUND,               "Stream not found",                 404,         906001) \
    XX(CODE_STREAM_OFFLINE,                 "Stream offline",                   400,         906002) \
    XX(CODE_STREAM_READER_ON_MSERVER_LIMITED, "Stream reader on media server reached limit ", 429, 906003) \
    XX(CODE_STREAM_READER_PER_CAMERA_LIMITED, "Stream reader on device reached limit", 429,  906004) \
    /* 907xxx - PTZ errors */                                                                        \
    XX(CODE_PTZ_PRESET_NOT_FOUND,           "User preset not found",            404,         907001) \
    XX(CODE_DEVICE_NO_SUPPORT_PTZ,          "Device does not support PTZ",      500,         907002) \
    XX(CODE_DEVICE_IS_RUNNING_PTZ,          "Device is running PTZ control",    500,         907003) \
    XX(CODE_PTZ_ABSOLUTED_CONTROL_FAILED,   "PTZ Absoluted control failed",     500,         907004) \
    XX(CODE_PTZ_CONTINUOUS_CONTROL_FAILED,  "PTZ Continuous control failed",    500,         907005) \
    XX(CODE_PTZ_RELATIVE_CONTROL_FAILED,    "PTZ Relative control failed",      500,         907006) \
    XX(CODE_DEVICE_CONFIG_DISABLE_PTZ,      "Device is configured to disable PTZ control", 403, 907007) \
    XX(CODE_DEVICE_NO_SUPPORT_SELECTED_PTZ_MODE, "Device does not support selected PTZ mode", 500, 907008) \
    XX(CODE_PTZ_GOTO_HOME_FAILED,           "PTZ Goto Home failed",             500,         907009) \
    XX(CODE_PTZ_GET_STATUS_FAILED,          "PTZ Get status failed",            500,         907010) \
    XX(CODE_PTZ_GOTO_PRESET_FAILED,         "PTZ Goto preset failed",           500,         907011) \
    XX(CODE_PTZ_GOTO_USER_PRESET_FAILED,    "PTZ Goto preset failed",           500,         907012) \
    XX(CODE_DEVICE_NO_SUPPORT_PTZ_PRESET,   "Device does not support PTZ Preset", 500,       907013) \
    /* 908xxx - Bookmark errors */                                                                   \
    XX(CODE_BOOKMARK_NOT_FOUND,             "Bookmark not found",               404,         908001) \
    XX(CODE_BOOKMARK_ID_NOT_FOUND,          "Bookmark id not found",            404,         908002) \
    XX(CODE_BOOKMARK_CREATE_FAILED,         "Bookmark creation failed",         500,         908003) \
    XX(CODE_BOOKMARK_UPDATE_FAILED,         "Bookmark update failed",           500,         908004) \
    XX(CODE_BOOKMARK_DELETE_FAILED,         "Bookmark delete failed",           500,         908005) \
    /* 909xxx - Extract errors */                                                                    \
    XX(CODE_EXTRACT_KEY_NOT_FOUND,          "Extract key not found",            404,         909001) \
    XX(CODE_EXTRACT_FAILED,                 "Extract video failed",             500,         909002) \
    /* 910xxx - Scan errors */                                                                       \
    XX(CODE_SCAN_KEY_NOT_FOUND,             "Scan key not found",               404,         910001) \
    XX(CODE_SUBNETSCAN_FAILED,              "Scan device by ip range failed",   500,         910002) \
    XX(CODE_SCAN_DEVICE_UNAUTHORIZED,       "Scan device unauthorized",         403,         910003) \
    XX(CODE_SCAN_DEVICE_HOST_UNREACHABLE,   "Scan device host unreachable",     403,         910004) \
    XX(CODE_SCAN_DEVICE_TIMEOUT,            "Scan device timeout",              403,         910005) \
    /* 911xxx - Health check errors */                                                               \
    XX(CODE_HEALTH_CHECK_API_FAILED,        "Health check api service failed",  500,         911001) \
    XX(CODE_HEALTH_CHECK_MSERVER_FAILED,    "Health check media server service failed",  500, 911002) \
    /* 912xxx - ONVIF/device control errors */                                                       \
    XX(CODE_ONVIF_SET_CONFIG_NOT_CHANGE,    "New config is the same as old config", 409,     912001) \
    XX(CODE_ONVIF_SET_CONFIG_FAILED,        "Failed to set new config",         500,         912002) \
    XX(CODE_ONVIF_GET_CONFIG_FAILED,        "Failed to get config",             500,         912003) \
    XX(CODE_DEVICE_NO_SUPPORT_IMAGING_FOCUS, "Device does not support focus control", 500,   912004) \
    XX(CODE_IMAGING_FOCUS_CONTROL_FAILED,   "Imaging focus control failed",     500,         912005) \
    XX(CODE_DEVICE_NO_SUPPORT_IMAGING_IRIS, "Device does not support iris control", 500,     912006) \
    XX(CODE_IMAGING_IRIS_CONTROL_FAILED,    "Imaging iris control failed",      500,         912007) \
    XX(CODE_DEVICE_NO_SUPPORT_RELAY_OUTPUT, "Device does not support relay output control", 500, 912008) \
    XX(CODE_RELAY_OUTPUT_FAILED,            "Relay output control failed",      500,         912009) \
    /* 913xxx - Sync database errors */                                                              \
    XX(CODE_SNAPSHOT_EMPTY,                 "Snapshot is empty",                404,         913001) \

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