#ifndef S3MANAGERKIT_XHOOK_H
#define S3MANAGERKIT_XHOOK_H

#include <string>
#include <functional>
#include "json/json.h"
#include "Server/util.h"

namespace xHook {
// Api url for getting configuration and reporting statistic
extern const std::string kApiUrl;
}//namespace xHook

void installxHook();
void unInstallxHook();
void onxProcessExited();

void addCameraResource(Json::Value &data, const std::function<void(const std::string &camera_id, const std::string &stream_id, const std::string &url)> &cb);
void delCameraResource(const std::string &resource_id, const std::function<void(const std::string &camera_id, const std::string &stream_id)> &cb);

#endif // S3MANAGERKIT_XHOOK_H
