#ifndef S3MANAGERKIT_MANAGER_H
#define S3MANAGERKIT_MANAGER_H

#include <string>
#include <functional>
#include "json/json.h"

namespace managerkit {

namespace Manager {
// Media server domain that management system regist for 
extern const std::string kMediaServerDomain;
// Maximum number of devices that service operation normally
extern const std::string kMaxAllowedDevices;
// Certification save path
extern const std::string kCertSavePath;
} // namespace Manager

} // namespace managerkit


void installManagerHook();

void unInstallManagerHook();

void migrateDatabase();

void loadServerConfigJson(const Json::Value &config);

void getServerStatisticJson(const std::function<void(Json::Value &data)> &cb);

#endif // S3MANAGERKIT_MANAGER_H
