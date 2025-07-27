#include "WebInterceptor.h"

using namespace std;

namespace API {

int getCustomCodeByCode(CustomCode code) {
    switch (code) {
#define XX(name, type, value, message, http_code, custom_code) case name : return custom_code;
        CUSTOM_CODE_MAP(XX)
#undef XX
        default : return 0;
    }
}

const char* getMessageByCode(CustomCode code) {
    switch (code) {
#define XX(name, type, value, message, http_code, custom_code) case name : return message;
        CUSTOM_CODE_MAP(XX)
#undef XX
        default : return "invalid";
    }
}

int getHttpCodeByCode(CustomCode code) {
    switch (code) {
#define XX(name, type, value, message, http_code, custom_code) case name : return http_code;
        CUSTOM_CODE_MAP(XX)
#undef XX
        default : return 0;
    }
}


} // namespace API
