#ifndef COMMON_STREAMSOURCE_H
#define COMMON_STREAMSOURCE_H

#include "Util/util.h"
#include "Player/PlayerProxy.h"
#include "Common/DeviceSource.h"
#include "Server/ResourceMonitor.h"

namespace managerkit {

typedef enum {
    PrimaryStream = 0,
    SecondaryStream = 1,
    StreamMax
} StreamType;

const std::string getStreamTypeString(int type);

bool isValidStreamType(int type);

struct StreamTuple : public DeviceTuple {
    std::string stream_id;
    std::string full_url;
    std::string shortUrl() const { 
        return vhost + '/' + device_id + '/' + stream_id; 
    }

    bool empty() const {
        return stream_id.empty() && full_url.empty();
    }

    bool operator==(const StreamTuple& other) const{
        return stream_id == other.stream_id &&
            full_url == other.full_url &&
            vhost == other.vhost &&
            device_id == other.device_id &&
            name == other.name;
    }
};

class StreamSource : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<StreamSource> {
public:
    using Ptr = std::shared_ptr<StreamSource>;

    StreamSource(int type, const StreamTuple &tuple, const mediakit::ProtocolOption &option, bool record_mp4 = false, int rtp_type = 0, int media_port = 0, std::string username = "", std::string password = "", float timeout_sec = 0.0f);

    ~StreamSource();

    void setListener(std::shared_ptr<DeviceSourceEvent> listener);

    void start();

    bool isLive() const { return _live.load(); }

    std::string getStatus() const {
        auto status_ptr = std::atomic_load_explicit(&_status, std::memory_order_acquire);
        return status_ptr ? *status_ptr : std::string();
    }

    mediakit::TranslationInfo getTranslationInfo();

    bool setupRecord(int type, bool start);

private:
    void setState(bool live, std::string status);

    void createPlayer();

    void closePlayer();

    void onStreamReady(bool ready, const std::string &status, const mediakit::TranslationInfo *info);

private:
    int _type;
    StreamTuple _tuple;
    mediakit::ProtocolOption _option;
    bool _record_mp4;
    int _rtp_type;
    int _media_port = 0;
    std::string _username;
    std::string _password;
    float _timeout_sec;
    std::string _full_url;
    std::atomic_bool _live {false};
    std::shared_ptr<const std::string> _status {std::make_shared<const std::string>("init")};
    std::weak_ptr<mediakit::PlayerProxy> _player;
};

} // namespace managerkit

#endif // COMMON_STREAMSOURCE_H