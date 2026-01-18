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

class StreamSource : public std::enable_shared_from_this<StreamSource> {
public:
    using Ptr = std::shared_ptr<StreamSource>;
    using OnStreamUpdate = std::function<void(bool live, const std::string &status, const mediakit::TranslationInfo *info)>;

    StreamSource(const StreamTuple &tuple, bool record = false, bool record_audio = true, int rtp_type = 0, int media_port = 0,
                    std::string username = "", std::string password = "", float timeout_sec = 0.0f);

    ~StreamSource();

    void start();

    bool isRecording() { return _record_mp4; }

    bool isRecordingAudio() { return _record_audio; }

    int getRtpType() { return _rtp_type; }

    int getMediaPort() { return _media_port; }

    void setOnStreamUpdate(const OnStreamUpdate &cb) { _on_update = std::move(cb); };

    bool isLive() { return _live.load(); }

    std::string getStatus() { return _status; }

    mediakit::TranslationInfo getTranslationInfo();

private:
    void createPlayer();

    void closePlayer();

private:
    StreamTuple _tuple;
    bool _record_mp4;
    bool _record_audio;
    int _rtp_type;
    int _media_port;
    std::string _username;
    std::string _password;
    float _timeout_sec;
    std::string _full_url;
    std::atomic_bool _live {false};
    std::string _status;
    mediakit::TranslationInfo _info;
    OnStreamUpdate _on_update;
    std::weak_ptr<mediakit::PlayerProxy> _player;
};

} // namespace managerkit

#endif // COMMON_STREAMSOURCE_H