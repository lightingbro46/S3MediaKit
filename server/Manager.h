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
// Enable user authorization when user want to access device data
extern const std::string kEnableAuthorize;
// Server location id, servers with the same one are considered to be in the same cluster
extern const std::string kServerLocationId;
// Enable failover mode to receive cameras from other server in same cluster
extern const std::string kEnableFailover;

} // namespace Manager

} // namespace managerkit


void installManagerHook();

void unInstallManagerHook();

void migrateDatabase();

void loadServerConfigJson(const Json::Value &config);

void getServerStatisticJson(const std::function<void(Json::Value &data)> &cb);

#endif // S3MANAGERKIT_MANAGER_H
