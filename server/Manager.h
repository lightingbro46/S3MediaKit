#ifndef S3MANAGERKIT_MANAGER_H
#define S3MANAGERKIT_MANAGER_H

#include <string>
#include <functional>
#include "json/json.h"

// void addCameraResource(Json::Value &data, const std::function<void(const std::string &camera_id, const std::string &stream_id, const std::string &url)> &cb);
// void delCameraResource(const std::string &resource_id, const std::function<void(const std::string &camera_id, const std::string &stream_id)> &cb);

void installManagerHook();

void unInstallManagerHook();

#endif // S3MANAGERKIT_MANAGER_H
