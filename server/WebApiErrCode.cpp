#include "WebApiErrCode.h"

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

} // namespace WebInterceptor
