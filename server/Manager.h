#ifndef S3MANAGERKIT_MANAGER_H
#define S3MANAGERKIT_MANAGER_H

#include <string>
#include <functional>
#include "json/json.h"
#include "Camera/CameraManager.h"

namespace managerkit {

namespace Manager {
// Media server domain that management system regist for 
extern const std::string kMediaServerDomain;
// Media server project id that management system regist for 
extern const std::string kMediaServerProjectId;
// Maximum number of devices that service operation normally
extern const std::string kMaxAllowedDevices;
// Maximum number of devices that service determined by the system CPU
extern const std::string kMaxAvailableDevices;
// Enable user authorization when user want to access device data
extern const std::string kEnableAuthorize;
// Server location id, servers with the same one are considered to be in the same cluster
extern const std::string kServerLocationId;
// Enable failover mode to receive cameras from other server in same cluster
extern const std::string kEnableFailover;
// Store public key to validate jwt token
extern const std::string kJwtPublicKey;
// User session expiration time (6 months)
extern const std::string kSessionExpiryDays;
// Maximum stream timeout seconds when connecting to device
extern const std::string kMaxStreamTimeoutSec;
// Bypass authentication realm, directly allow access
extern const std::string kBypassAuthRealm;

} // namespace Manager

} // namespace managerkit

extern std::string g_ini_file;

void installManagerHook();

void unInstallManagerHook();

void migrateDatabase();

void loadServerConfigJson(const Json::Value &config);

void getServerStatisticJson(const std::function<void(Json::Value &data)> &cb);

void getServerUsageJson(const std::function<void(Json::Value &data)> &cb);

void loadServerStartedConfigJson(const Json::Value &data);

Json::Value makeDeviceCapabilitiesJson(const managerkit::DeviceSource::Ptr &device, const managerkit::DeviceCapabilities* caps);

Json::Value makeSystemStatisticJson();

Json::Value makeSystemStorageJson();

Json::Value makeStorageStatisticJson();

int estimateMaxAvailableDevice();

void countDeviceStatusJson(const Json::Value &data, int &online, int &offline);

void installGlobalMonitor();

Json::Value makeDeviceStatisticJson(const managerkit::DeviceSource::Ptr &device);

Json::Value makeDevicePTZPresetJson(const managerkit::DeviceSource::Ptr &device);

#endif // S3MANAGERKIT_MANAGER_H
