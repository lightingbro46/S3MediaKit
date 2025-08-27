#ifndef  S3PLUGINKIT_FACTORY_H
#define  S3PLUGINKIT_FACTORY_H

#include <string>
#include "Util/onceToken.h"

#define REGISTER_STATIC_VAR_INNER_(var_name, line) var_name##_##line##__
#define REGISTER_STATIC_VAR_(var_name, line) REGISTER_STATIC_VAR_INNER_(var_name, line)

#define REGISTER_DRIVER(plugin) \
extern DriverPlugin plugin;     \
static toolkit::onceToken REGISTER_STATIC_VAR_(s_token, __LINE__) ([]() { \
    DriverFactory::registerPlugin(plugin); \
});

namespace managerkit {

std::string replaceIp(const std::string &in_url, const std::string &nat_ip);

std::string replacePort(const std::string &in_url, int nat_port);

bool eligibleForPrimaryStream(std::string vcodec, int width, int height, int bitrate, int fps);

bool eligibleForSecondaryStream(std::string vcodec, int width, int height, int bitrate, int fps);

// typedef enum {
//     DriverInvalid = -1,
//     DriverController = 0,
//     DriverAnalystic,
// } DriverType;

// #define DRIVER_MAP(XX) \
//     XX(DriverOnvif, DriverController, 0, "ONVIF")

// typedef enum {
//     DriverInvalid = -1,
// #define XX(name, type, value, str) name = value,
//     DRIVER_MAP(XX)
//     DriverMax
// #undef XX
// } DriverId;

// struct DriverPlugin {
//     DriverId (*getPlugin)();
//     Controller::Ptr (*getControllerByPluginId)(std::string ip, int port, std::string username, std::string password);
// };

// class DriverFactory {
// public:
//     /**
//      * Register plugin, not thread-safe
//      */
//     static void registerPlugin(const DriverPlugin &plugin);

//     /**
//      * Get controller by driver id
//      */
//     static Controller::Ptr getControllerByDriverId(DriverId driverId, std::string ip = "localhost", int port = 80, std::string username = "", std::string password = "");
// };

} // namespace managerkit 

#endif // S3PLUGINKIT_FACTORY_H