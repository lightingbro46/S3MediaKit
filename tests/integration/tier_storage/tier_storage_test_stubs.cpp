#include "Manager.h"
#include "WebApi.h"

std::string g_ini_file;

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
