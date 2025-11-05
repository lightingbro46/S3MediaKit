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
};

bool equalStreamTuple(const StreamTuple &a, const StreamTuple &b);

class StreamSource : public std::enable_shared_from_this<StreamSource> {
public:
    using Ptr = std::shared_ptr<StreamSource>;

    StreamSource(const StreamTuple &tuple, bool record = false, int rtp_type = 0, int media_port = 0, float timeout_sec = 0.0f);

    ~StreamSource();

    void start();

    bool isRecording() { return _record; }

    int getRtpType() { return _rtp_type; }

    int getMediaPort() { return _media_port; }

    void setOnStreamReady(const std::function<void()> &cb) { _on_ready = std::move(cb); };

    void setOnStreamChange(const std::function<void()> &cb) { _on_change = std::move(cb); };

    bool isLive() { return _live.load(); }

    std::string getStatus() { return _status; }

    mediakit::TranslationInfo getTranslationInfo();

private:
    void createPlayer();

    void closePlayer();

private:
    StreamTuple _tuple;
    bool _record;
    int _rtp_type;
    int _media_port;
    float _timeout_sec;
    std::string _full_url;
    std::atomic_bool _live {false};
    std::string _status;
    mediakit::TranslationInfo _info;
    std::function<void()> _on_ready;
    std::function<void()> _on_change;
    std::weak_ptr<mediakit::PlayerProxy> _player;
};

} // namespace managerkit

#endif // COMMON_STREAMSOURCE_H