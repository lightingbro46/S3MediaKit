#include "WebApiErrCode.h"
#include "User/UserSessionCache.h"

using namespace std;

namespace managerkit {

bool isValidApiErrCode(int code) {
    switch (code) {
#define XX(type, message, http_code, custom_code) case type : return true;
        API_ERROR_CODE_MAP(XX)
#undef XX
        default : return false;
    }
}

const char* getDefaultMessage(ApiErrCode code) {
    switch (code) {
#define XX(type, message, http_code, custom_code) case type : return message;
        API_ERROR_CODE_MAP(XX)
#undef XX
        default : return "invalid code";
    }
}

int getStatusCode(ApiErrCode code) {
    switch (code) {
#define XX(type, message, http_code, custom_code) case type : return http_code;
        API_ERROR_CODE_MAP(XX)
#undef XX
        default : return -1;
    }
}

ApiErrCode getApiErrCodeWithPermission(const std::string &code) {
    if (code == LIVE_VIEW_PERMISSION_CODE) {
        return ApiErrCode::CODE_NO_LIVE_VIEW_PERMISSION;
    } else if (code == PLAYBACK_PERMISSION_CODE) {
        return ApiErrCode::CODE_NO_PLAYBACK_PERMISSION;
    } else if (code == PTZ_CONTROL_PERMISSION_CODE) {
        return ApiErrCode::CODE_NO_PTZ_CONTROL_PERMISSION;
    } else if (code == READ_MSERVER_PERMISSION_CODE) {
        return ApiErrCode::CODE_NO_READ_MSERVER_PERMISSION;
    } else if (code == MODIFY_MSERVER_PERMISSION_CODE) {
        return ApiErrCode::CODE_NO_MODIFY_MSERVER_PERMISSION;
    } else if (code == ADD_CAMERA_PERMISSION_CODE) {
        return ApiErrCode::CODE_NO_ADD_CAMERA_PERMISSION;
    } else {
        return ApiErrCode::CODE_PERMISSION_DENIED;
    }
}

} // namespace WebInterceptor
