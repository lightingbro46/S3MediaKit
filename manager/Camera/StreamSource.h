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

// All configuration needed to create and identify a StreamSource instance.
struct StreamOption {
    StreamTuple tuple;
    mediakit::ProtocolOption protocol;
    bool record_mp4   = false;
    int  rtp_type     = 0;
    int  media_port   = 0;
    std::string username;
    std::string password;
    float timeout_sec = 0.0f;

    bool operator==(const StreamOption &o) const {
        return tuple                   == o.tuple
            && record_mp4             == o.record_mp4
            && rtp_type               == o.rtp_type
            && media_port             == o.media_port
            && username               == o.username
            && password               == o.password
            && protocol.enable_audio  == o.protocol.enable_audio
            && protocol.enable_motion == o.protocol.enable_motion
            && protocol.roi_mask      == o.protocol.roi_mask
            && protocol.record_motion == o.protocol.record_motion;
    }
};

class StreamSource : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<StreamSource> {
public:
    using Ptr = std::shared_ptr<StreamSource>;

    StreamSource(int type, const StreamOption &option);

    ~StreamSource();

    void setListener(std::shared_ptr<DeviceSourceEvent> listener);

    void start();

    bool isLive() const { return _live.load(); }

    const StreamOption &getOption() const { return _option; }

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
    StreamOption _option;
    std::string _full_url;
    std::atomic_bool _live {false};
    std::shared_ptr<const std::string> _status {std::make_shared<const std::string>("init")};
    std::weak_ptr<mediakit::PlayerProxy> _player;
};

} // namespace managerkit

#endif // COMMON_STREAMSOURCE_H