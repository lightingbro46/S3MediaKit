#ifndef S3MANAGERKIT_XHOOK_H
#define S3MANAGERKIT_XHOOK_H

#include <string>
#include <functional>
#include "json/json.h"

void addCameraResource(Json::Value &data, const std::function<void(const std::string &camera_id, const std::string &stream_id, const std::string &url)> &cb);
void delCameraResource(const std::string &resource_id, const std::function<void(const std::string &camera_id, const std::string &stream_id)> &cb);

#endif // S3MANAGERKIT_XHOOK_H
