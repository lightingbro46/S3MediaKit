#include "Plugin.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Common/strCoding.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

bool eligibleForPrimaryStream(string vcodec, int width, int height, int bitrate, int fps) {
    // todo: select by width, height and bitrate
    if (vcodec == "H264" && height >= 1080) {
        return true;
    }
    return false;
}

bool eligibleForSecondaryStream(string vcodec, int width, int height, int bitrate, int fps) {
     if (vcodec == "H264" && height <= 480) {
        return true;
    }
    return false;
}

// static std::unordered_map<int, const DriverPlugin *> s_plugins;

// REGISTER_DRIVER(onvif_plugin)

// void DriverFactory::registerPlugin(const DriverPlugin &plugin) {
//     InfoL << "Load driver: " << getDriverName(plugin.getDriver());
//     s_plugins[(int)(plugin.getDriver())] = &plugin;
// }

// Controller::Ptr DriverFactory::getControllerByDriverId(DriverId driverId, string ip, int port, string username, string password) {
//     auto it = s_plugins.find(driverId);
//     if (it == s_plugins.end()) {
//         return nullptr;
//     }
//     return it->second->getControllerByDriverId(ip, port, username, password);
// }

} // namespace managerkit
