#include "Manager.h"
#include "WebApi.h"

std::string g_ini_file;

namespace managerkit {
namespace Manager {

const std::string kMediaServerDomain = "manager.mediaServerDomain";
const std::string kMediaServerProjectId = "manager.mediaServerProjectId";
const std::string kMaxAllowedDevices = "manager.maxAllowedDevices";
const std::string kMaxAvailableDevices = "manager.maxAvailableDevices";
const std::string kEnableAuthorize = "manager.enableAuthorize";
const std::string kServerLocationId = "manager.serverLocationId";
const std::string kEnableFailover = "manager.enableFailover";
const std::string kFailoverActiveDelaySec = "manager.failoverActiveDelaySec";
const std::string kJwtPublicKey = "manager.jwtPublicKey";
const std::string kSessionExpiryDays = "manager.sessionExpiryDays";
const std::string kMaxStreamTimeoutSec = "manager.maxStreamTimeoutSec";
const std::string kBypassAuthRealm = "manager.bypassAuthRealm";
const std::string kApiSecret = "manager.apiSecret";

} // namespace Manager
} // namespace managerkit

void addStreamProxy(const mediakit::MediaTuple &,
                    const mediakit::ProtocolOption &,
                    const std::function<void(const std::string &, const mediakit::PlayerProxy::Ptr &)> &cb) {
    cb("Disabled in tier-storage integration test", nullptr);
}

void delStreamProxy(const mediakit::MediaTuple &) {}

void addReplayStreamProxy(const mediakit::MediaTuple &,
                          const mediakit::ProtocolOption &,
                          const std::function<void(const std::string &, const mediakit::PlayerProxy::Ptr &)> &cb,
                          int) {
    cb("Disabled in tier-storage integration test", nullptr);
}

const mediakit::PlayerProxy::Ptr getReplayStreamProxy(const std::string &) {
    return nullptr;
}

void delReplayStreamProxy(const std::string &) {}
