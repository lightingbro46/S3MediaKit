#ifndef S3MEDIAKIT_WEBHOOK_H
#define S3MEDIAKIT_WEBHOOK_H

#include <string>
#include <functional>
#include "json/json.h"

// // Support json or urlencoded way to transmit parameters
#define JSON_ARGS

#ifdef JSON_ARGS
typedef Json::Value ArgsType;
#else
typedef mediakit::HttpArgs ArgsType;
#endif

namespace Hook {
// Maximum timeout for web hook reply
extern const std::string kTimeoutSec;
}//namespace Hook

void installWebHook();
void unInstallWebHook();
void onProcessExited();
/**
 * Trigger http hook request
 * @param url Request address
 * @param body Request body
 * @param func Callback
 */
void do_http_hook(const std::string &url, const ArgsType &body, const std::function<void(const Json::Value &, const std::string &)> &func = nullptr);
#endif //S3MEDIAKIT_WEBHOOK_H
