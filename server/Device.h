#ifndef S3MEDIAKIT_DEVICE_H
#define S3MEDIAKIT_DEVICE_H

#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>

#include "Util/TimeTicker.h"
#include "Poller/Timer.h"
#include "Poller/EventPoller.h"
#include "Common/macros.h"

#define PRIMARY_STREAM_SCHEMA "Primary"
#define SECONDARY_STREAM_SCHEMA "Secondary"
#define SINGLE_STREAM_SCHEMA 1
#define DUAL_STREAM_SCHEMA 2

#define RTSP_PROTOCOL "rtsp"
#define RTSPS_PROTOCOL "rtsps"
#define RTMP_PROTOCOL "rtmp"
#define RTMPS_PROTOCOL "rtmps"

#define RTP_TRANSPORT_TCP_SCHEMA "tcp"
#define RTP_TRANSPORT_UDP_SCHEMA "udp"
#define RTP_TRANSPORT_MUTICAST_SCHEMA "muticast"

std::string getGuid(std::string &id);

bool isProtocolSupport(std::string &protocol);

namespace manager
{

struct Resource {
    std::string guid;
    Resource () {
        guid = "";
    }
};

struct DeviceTuple: Resource{
    std::string id;
    bool enable;
    std::string ip;
    int port;
    int sslport;
    int rtsp_port;
    int rtsp_sslport;
    int rtmp_port;
    int rtmp_sslport;
    std::string username;
    std::string password;
    std::string manufacturer;
    std::string model;
    std::string area;

    DeviceTuple() {
        id = "";
        enable = false;
        ip = "";
        port = 0;
        sslport = 0;
        rtsp_port = 0;
        rtsp_sslport = 0;
        rtmp_port = 0;
        rtmp_sslport = 0;
        username = "";
        password = "";
        manufacturer = "";
        model = "";
        area = "";
    }
};

struct StreamTuple : Resource {
    std::string id;
    bool enable;
    std::string name;
    std::string protocol;
    std::string rtp_transport;
    std::string path;
    bool is_live;
    bool is_record;
    int record_min;
    int record_max;
    std::vector<std::vector<std::string>> record_schedule;
    std::shared_ptr<DeviceTuple> device;
    std::string getUrl() const {
        if (protocol.empty()) {
            WarnL << "Protocol of stream "<< id << " is empty";
            return "";
        }
        
        // if (!isProtocolSupport(protocol)) {
        //     WarnL << "Protocol " << protocol << " of stream "<< id << " do not support";
        //     return "";
        // }

        std::string url = "";
        url = protocol + "://";
        if (!device->username.empty()) {
            url += device->username + ":" + device->password + "@";
        }
        url += device->ip;
        if (protocol == RTSP_PROTOCOL) {
            url += device->rtsp_port != 0 ? ":" + std::to_string(device->rtsp_port) : "";
        } else if (protocol == RTSPS_PROTOCOL) {
            url += device->rtsp_sslport != 0 ? ":" + std::to_string(device->rtsp_sslport) : "";
        } else if (protocol == RTMP_PROTOCOL) {
            url += device->rtmp_port != 0 ? ":" + std::to_string(device->rtmp_port) : "";
        } else if (protocol == RTMPS_PROTOCOL) {
            url += device->rtmp_sslport != 0 ? ":" + std::to_string(device->rtmp_sslport) : "";
        }
        return url + path;
    }

    StreamTuple() {
        id = "";
        name = "";
        enable = false;
        protocol = "";
        rtp_transport= "tcp";
        path = "";
        is_live = false;
        is_record = false;
        record_min = 0;
        record_max = 0;
        record_schedule = {};
        device.reset();
    }

    ~StreamTuple() {
        device.reset();
    }
};

class DeviceProxy : public std::enable_shared_from_this<DeviceProxy> {
public:
    using Ptr = std::shared_ptr<DeviceProxy>;

    DeviceProxy(const DeviceTuple &tuple, std::vector<StreamTuple> streams = std::vector<StreamTuple>(),
        int retry_count = -1, const toolkit::EventPoller::Ptr &poller = nullptr,
        int reconnect_delay_min = 2, int reconnect_delay_max = 60, int reconnect_delay_step = 3);

    ~DeviceProxy();

    void setOnConnect(std::function<void(const std::string &)> cb);
    void setOnDisconnect(std::function<void()> cb);

    void connect();

    const DeviceTuple &getDeviceTuple() const { return _tuple; }
    const StreamTuple getStream(const std::string &name) const  {
        auto it = std::find_if(_streams.begin(), _streams.end(), [&](const StreamTuple &stream) {
            return stream.name == name;
        });
        if (it != _streams.end()) {
            return *it;
        } else {
            throw std::runtime_error("Stream not found");
        }
    };

    bool hasStreamTuple(const std::string &name) const {return _streams.size() > 0; }
    bool isDualStreamTuple() const { return _streams.size() == 2; };

private:
    void disconnect();

private:
    int _retry_count;
    int _reconnect_delay_min;
    int _reconnect_delay_max;
    int _reconnect_delay_step;
    DeviceTuple _tuple;
    std::vector<StreamTuple> _streams;
    toolkit::Timer::Ptr _timer;
    std::function<void(const std::string &)> _on_connect;
    std::function<void()> _on_disconnect;

    toolkit::Ticker _live_ticker;
    // 0 indicates normal, 1 indicates attempting to stream
    std::atomic<int> _live_status;
    std::atomic<uint64_t> _live_secs;

    std::atomic<uint64_t> _reconnect_count;
};
} // namespace mediakit                                                       

#endif // S3MEDIAKIT_DEVICE_H