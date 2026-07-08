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
    XX(CODE_INVALID_WATERMARK_PERCENT,      "Invalid watermark percent",        404,         902008) \
    XX(CODE_INVALID_TIME_RANGE,             "Invalid time range",               404,         902009) \
    XX(CODE_ENDPOINT_NOT_SUPPORTED,         "Endpoint not supported",           404,         902010) \
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
    /* 914xxx - Storage tier errors */                                                              \
    XX(CODE_STORAGE_POOL_NOT_FOUND,         "Storage pool not found",          404,         914001) \
    XX(CODE_STORAGE_POLICY_NOT_FOUND,       "Storage policy not found",        404,         914002) \
    XX(CODE_STORAGE_POOL_IN_USE,            "Storage pool in use",             404,         914003) \
    XX(CODE_STORAGE_POOL_OFFLINE,           "Storage pool offline",            404,         914004) \
    XX(CODE_STORAGE_POOL_CONN_FAILED,       "Storage pool connection failed",  404,         914005) \
    XX(CODE_STORAGE_POLICY_IN_USE,          "Storage policy in use",           404,         914006) \
    XX(CODE_STORAGE_POLICY_INVALID,         "Storage policy invalid",          404,         914007) \
    XX(CODE_STORAGE_TIER_ORDER_INVALID,     "Storage tier order invalid",      404,         914008) \
    XX(CODE_STORAGE_RESTORE_REQUIRED,       "Storage restore required",        404,         914009) \
    XX(CODE_STORAGE_POOL_CREATE_FAILED,     "Storage pool creation failed",    500,         914010) \
    XX(CODE_STORAGE_POOL_UPDATE_FAILED,     "Storage pool update failed",      500,         914011) \
    XX(CODE_STORAGE_POOL_DELETE_FAILED,     "Storage pool delete failed",      500,         914012) \
    XX(CODE_STORAGE_POLICY_CREATE_FAILED,   "Storage policy creation failed",  500,         914013) \
    XX(CODE_STORAGE_POLICY_UPDATE_FAILED,   "Storage policy update failed",    500,         914014) \
    XX(CODE_STORAGE_POLICY_DELETE_FAILED,   "Storage policy delete failed",    500,         914015) \
    XX(CODE_STORAGE_SEGMENTS_NOT_FOUND,     "Storage segments not found",      404,         914016) \
    XX(CODE_STORAGE_SEGMENTS_EXPIRED,       "Storage segments expired",        404,         914017) \
    XX(CODE_STORAGE_RESTORE_JOB_NOT_FOUND,  "Storage restore job not found",   404,         914018) \
    XX(CODE_STORAGE_RESTORE_JOB_CREATION_FAILED, "Restore job creation failed", 500,        914019) \
    XX(CODE_STORAGE_TIERING_JOB_NOT_FOUND,  "Storage tiering job not found",   404,         914020) \
    XX(CODE_STORAGE_TIERING_JOB_UPDATE_FAILED, "Storage tiering job update failed", 500,    914021) \
    XX(CODE_STORAGE_TIERING_JOB_CANCEL_FAILED, "Storage tiering job cancel failed", 500,    914022) \
    XX(CODE_STORAGE_ALERT_NOT_FOUND,        "Storage alert not found",         404,         914023) \
    XX(CODE_STORAGE_SEGMENT_PROTECTED_NOT_FOUND, "Storage segment protected not found", 404, 914024) \
    XX(CODE_STORAGE_SEGMENT_PROTECTED_CREATE_FAILED, "Storage segment protected create failed", 500, 914025) \
    XX(CODE_STORAGE_POLICY_CLONE_FAILED,   "Storage policy clone failed",      500,         914026) \
    XX(CODE_STORAGE_POLICY_ASSIGN_CAMERA_FAILED, "Storage policy assign to camera failed", 500, 914027) \
    XX(CODE_STORAGE_POLICY_UNASSIGN_CAMERA_FAILED, "Storage policy unassign from camera failed", 500, 914028) \
    XX(CODE_STORAGE_POOL_DEFAULT_CANNOT_DELETE, "Storage pool default cannot be deleted",  400, 914029) \
    XX(CODE_STORAGE_POLICY_DEFAULT_CANNOT_DELETE, "Storage policy default cannot be deleted", 400, 914030) \
    /* 915xxx - Audio errors */                                                                        \
    XX(CODE_AUDIO_FILE_NOT_FOUND,           "Audio file not found",             404,         915001) \
    XX(CODE_PLAY_AUDIO_FAILED,              "Failed to play audio file",        500,         915002) \

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