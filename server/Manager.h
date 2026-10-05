#ifndef S3MANAGERKIT_MANAGER_H
#define S3MANAGERKIT_MANAGER_H

#include <string>
#include <functional>
#include "json/json.h"
#include "Camera/CameraManager.h"
#include "Speaker/SpeakerManager.h"
#include "Speaker/AudioFileManager.h"

extern std::string g_ini_file;

void installManagerHook();

void unInstallManagerHook();
// Stops the storage managers (tier cycles, object storage SDK). Part of unInstallManagerHook();
// also called when the server fails to start.
void releaseStorageManagers();

void migrateDatabase();

void loadServerConfigJson(const Json::Value &config);

void getServerStatisticJson(const std::function<void(Json::Value &data)> &cb);

void getServerUsageJson(const std::function<void(Json::Value &data)> &cb);

void loadServerStartedConfigJson(const Json::Value &data);

Json::Value makeDeviceCapabilitiesJson(const managerkit::DeviceSource::Ptr &device, const managerkit::DeviceCapabilities* caps);

Json::Value makeDeviceStoragesJson();

void countDeviceStatusJson(const Json::Value &data, int &online, int &offline);

void installGlobalMonitor();

Json::Value makeDeviceStatisticJson(const managerkit::DeviceSource::Ptr &device);

Json::Value makeDevicePTZPresetJson(const managerkit::DeviceSource::Ptr &device);

Json::Value makeDeviceMediaProfileJson(const managerkit::DeviceSource::Ptr &device);

managerkit::DeviceSource::Ptr findDeviceSource(const std::string &deviceId, const std::string &schema = "");

Json::Value makePrimaryMediaProfileJson(const managerkit::DeviceSource::Ptr &device);

#endif // S3MANAGERKIT_MANAGER_H
