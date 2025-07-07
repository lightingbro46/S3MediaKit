#ifndef S3MEDIAKIT_WEBERRORCODE_H
#define S3MEDIAKIT_WEBERRORCODE_H

#include <string>
#include "Network/Socket.h"

namespace API {

#define CUSTOM_CODE_MAP(XX) \
    XX(CODE_SUCCESS, ApiErr::Success, 0, "Success", 200, 0)

typedef enum {
#define XX(name, type, value, message, http_code, custom_code) name = value,
    CUSTOM_CODE_MAP(XX)
#undef XX

} CustomCode;

} // namespace API

/**
 * Get custom code id by codeid
 */
int getCustomCodeByCode(API::CustomCode code = API::CustomCode::CODE_SUCCESS);

/**
 * Get custom message by codeid
 */
const char* getMessageByCode(API::CustomCode code = API::CustomCode::CODE_SUCCESS);

/**
 * Get http code by codeid
 */
int getHttpCodeByCode(API::CustomCode code = API::CustomCode::CODE_SUCCESS);


#endif // S3MEDIAKIT_WEBERRORCODE_H