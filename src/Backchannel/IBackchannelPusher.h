#ifndef BACKCHANNEL_IBACKCHANNEL_PUSHER_H
#define BACKCHANNEL_IBACKCHANNEL_PUSHER_H

#include <functional>
#include <memory>
#include <string>
#include "Network/Socket.h"
#include "BackchannelMediaSource.h"

namespace mediakit {

// Where and how a device takes the audio BackchannelManager plays to it.
struct AudioBackchannelInfo {
    enum Transport { NONE, RTSP_ONVIF, HTTP_ISAPI };
    Transport transport = NONE;
    // RTSP_ONVIF: the backchannel RTSP URI, credentials inside.
    // HTTP_ISAPI: http://ip[:port]/ISAPI/System/TwoWayAudio/channels/<id>, credentials in the fields below.
    std::string url;
    std::string username;
    std::string password;
    std::string codec;    // HTTP_ISAPI: "PCMU" or "PCMA"; RTSP_ONVIF negotiates it in the SDP
    int sampleRate = 8000;
};

// One audio session to one device; BackchannelManager drives the RTSP (ONVIF) and HTTP (ISAPI)
// pushers through this.
class IBackchannelPusher {
public:
    using Ptr = std::shared_ptr<IBackchannelPusher>;

    virtual ~IBackchannelPusher() = default;

    virtual void start() = 0;
    virtual void attachSource(BackchannelMediaSource::Ptr source) = 0;
    virtual void detachSource() = 0;
    // Ends the session on the device; safe from any thread.
    virtual void stopSession() = 0;

    // Fired once, when the device accepted the session.
    std::function<void()> onReady;
    // Fired once, if the session could not be set up.
    std::function<void(const toolkit::SockException &)> onFailed;
    // Fired if the session drops unexpectedly *after* onReady already fired.
    std::function<void(const toolkit::SockException &)> onShutdown;
    std::function<void()> onSourceFinished;
};

} // namespace mediakit

#endif // BACKCHANNEL_IBACKCHANNEL_PUSHER_H
