#include "Device.h"

using namespace std;

std::string getGuid(std::string &id) {
    std::string result = id;
    result.erase(std::remove(result.begin(), result.end(), '-'), result.end());
    return result;
}

bool isProtocolSupport(std::string &protocol) {
    std::vector<std::string> supported_protocols = {
        RTSP_PROTOCOL,
        RTSPS_PROTOCOL,
        RTMP_PROTOCOL,
        RTMPS_PROTOCOL
    };
    if (std::find(supported_protocols.begin(), supported_protocols.end(), protocol) != supported_protocols.end()) {
        return true;
    } else {
        return false;
    }
} 

namespace manager
{
DeviceProxy::DeviceProxy(const DeviceTuple &tuple, std::vector<StreamTuple> streams, int retry_count,
    const toolkit::EventPoller::Ptr &poller, int reconnect_delay_min, int reconnect_delay_max, int reconnect_delay_step)
    : _tuple(tuple), _streams(streams) {
    _retry_count = retry_count;

    setOnConnect(nullptr);
    setOnDisconnect(nullptr);

    _reconnect_delay_min = reconnect_delay_min > 0 ? reconnect_delay_min : 2;
    _reconnect_delay_max = reconnect_delay_max > 0 ? reconnect_delay_max : 60;  
    _reconnect_delay_step = reconnect_delay_step > 0 ? reconnect_delay_step : 3;
    _live_secs = 0;
    _live_status = 1;
    _reconnect_count = 0;
}

DeviceProxy::~DeviceProxy() {
    disconnect();
}

void DeviceProxy::connect() {
    // todo: connect by ONVIF by protocol
    // todo: add stream proxy
    // Call _on_connect when connected
}

void DeviceProxy::disconnect() {
    //todo: disconnect by ONVIF
    // todo: del stream proxy
    // Call _on_disconnect when disconnected
}

void DeviceProxy::setOnConnect(std::function<void(const std::string &)> cb) {
    _on_connect = cb ? std::move(cb) : [](const std::string &) {};
}

void DeviceProxy::setOnDisconnect(std::function<void()> cb) {
    _on_disconnect = cb ? std::move(cb) : [] () {};
}

} // namespace manager
