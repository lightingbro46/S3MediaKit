#ifndef S3MANAGERKIT_CAMERA_H
#define S3MANAGERKIT_CAMERA_H

#include "Resource.h"
#include <mutex>
#include <vector>
#include <functional>
#include <string>
#include <unordered_map>

#define HTTP_PROTOCOL_SCHEMA "http"
#define HTTPS_PROTOCOL_SCHEMA "https"
#define RTSP_PROTOCOL_SCHEMA "rtsp"
#define RTSPS_PROTOCOL_SCHEMA "rtsps"
#define RTMP_PROTOCOL_SCHEMA "rtmp"
#define RTMPS_PROTOCOL_SCHEMA "rtmps"

namespace managerkit
{

struct CameraOption {
    int port = 0;
    bool use_https = false;
    int sslport = 443;
    int rtsp_port = 0;
    int rtsp_sslport = 0;
    int rtmp_port = 0;
    int rtmp_sslport = 0;
    std::string rtp_transport;
    std::string name;
    std::string ip;
    std::string path;
    std::string username;
    std::string password;
    std::string manufacturer;
    std::string model;
    std::string area;
    bool enable = false;
};

class CameraOption {
public:
    std::string credentials;
    std::string driverClass;
    std::string firmware;
    bool isAudioSupported;
    
};

class CameraResource : public ResourceSinkInterface {
public:
    using Ptr = std::shared_ptr<CameraResource>;
  
    CameraResource(const std::string &guid, const CameraTuple &tuple, const CameraOption &option);

    ~CameraResource() override;

    void connect();
    void setOnConnect(std::function<void(const std::string &)> cb) {
        _on_connect = cb ? std::move(cb) : nullptr;
    }
    void setOnDisconnect(std::function<void()> cb) {
        _on_disconnect = cb ? std::move(cb) : nullptr;
    }
private:
    void disconnect();

private:

    std::recursive_mutex _mx;
    std::string _guid;

    std::unordered_map<std::string, std::string> _option;

    std::function<void(const std::string &)> _on_connect;
    std::function<void()> _on_disconnect;
};

} // namespace managerkit

struct StreamTuple {
    std::string protocol{};
    std::string rtp_transport{};
    std::string id{};
    std::string name{};
    std::string path {};
    bool enable = false;
};

class StreamProxy;
class CameraProxy;

struct StreamParam {
    using Ptr = std::shared_ptr<StreamParam>;

    int width = 0;
    int height = 0;
    int bitrate = 0;
    int fps = 0;
    bool status = false;

    std::string id{};
    std::weak_ptr<StreamProxy> stream;

    ~StreamParam() {};
};

class StreamProxy: public ResourceSinkInterface {
public:
    using Ptr = std::shared_ptr<StreamProxy>;

    StreamProxy(const std::string &guid, const std::string &name, const std::string &url, const std::string &camera_id, StreamTuple &tuple);
    ~StreamProxy();

    void addParam(std::weak_ptr<StreamParam> &param) {
        std::lock_guard<std::recursive_mutex> lock(_mx);
        _param = param;
    }

private:
    std::recursive_mutex _mx;
    StreamTuple _tuple;

    std::weak_ptr<StreamParam> _param;
};

struct CameraTuple {
    int port = 0;
    bool use_https = false;
    int sslport = 443;
    int rtsp_port = 0;
    int rtsp_sslport = 0;
    int rtmp_port = 0;
    int rtmp_sslport = 0;
    std::string id{};
    std::string name{};
    std::string ip{};
    std::string path{};
    std::string username{};
    std::string password{};
    std::string manufacturer{};
    std::string model{};
    std::string area{};
    bool enable = false;
};

class CameraProxy : public Resource {
public:
    using Ptr = std::shared_ptr<CameraProxy>;

    CameraProxy(const std::string &guid, const std::string &name, const std::string url, const std::string mediaserver_id, const CameraTuple &tuple);
    ~CameraProxy();

    void setOnConnect(std::function<void(const std::string &)> cb) {
        _on_connect = cb ? std::move(cb) : nullptr;
    }
    void setOnDisconnect(std::function<void()> cb) {
        _on_disconnect = cb ? std::move(cb) : nullptr;
    }

    void connect();
    void addStream(const std::weak_ptr<StreamProxy> &stream) {
        std::lock_guard<std::recursive_mutex> lock(_mx);
        _streams.emplace_back(stream);
    }

private:
    void disconnect();

private:
    std::recursive_mutex _mx;
    std::string _id;
    CameraTuple _tuple;
    std::vector<std::weak_ptr<StreamProxy>> _streams;

    std::function<void(const std::string &)> _on_connect;
    std::function<void()> _on_disconnect;
};

std::string getUrl(const CameraTuple &camera);

std::string getStreamUrl(const CameraTuple &camera, const StreamTuple &stream);

namespace managerkit
{

class CameraResourceOption : public ResourceOption {
public:
    using Ptr = std::shared_ptr<CameraResourceOption>;
    CameraResourceOption() = default;
    ~CameraResourceOption() override;

    bool dontRecordAudio = false;

    bool dontRecordPrimaryStream = false;

    bool dontRecordSecondaryStream = false;

    bool forcedPrimaryProfile = false;

    bool forcedSecondaryProfile = false;

    int http_port;

    bool isAudioSupported = true;

    bool keepCameraTimeSettings = true;

    std::string rtp_transport;

    bool hasDualStreaming = true;

    Json::Value streamUrls;

    bool trustCameraTime = false;

    bool twoWayAudioEnabled = false;

    std::string useMedia2ToFetchProfiles = "autoSelect";

    // Maximum number of streams
    size_t max_stream = 2;

    
};

class CameraResource : public Resource {
public:
    using RingType = toolkit::RingBuffer<std::string>;
    using Ptr = std::shared_ptr<CameraResource>;

    CameraResource(const std::string &schema, const ResourceTuple &tuple) : Resource(schema, tuple) {}

    /**
     * 	Get the circular buffer of the resource
     */
    const RingType::Ptr &getRing() const { return _ring; }

    /**
     * Get the number of players
     */
    int readerCount() override { return _ring ? _ring->readerCount() : 0; }

    void getPlayerList(const std::function<void(const std::list<toolkit::Any> &info_list)> &cb,
                       const std::function<toolkit::Any(toolkit::Any &&info)> &on_change) override {
        _ring->getInfoList(cb, on_change);
    }

private:
    RingType::Ptr _ring;
    mutable std::mutex _mtx_index;
    toolkit::List<std::function<void(const std::string &)>> _list_cb;
}
    
} // namespace managerkit

#endif // S3MANAGERKIT_CAMERA_H