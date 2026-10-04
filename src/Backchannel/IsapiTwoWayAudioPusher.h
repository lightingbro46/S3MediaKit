#ifndef BACKCHANNEL_ISAPI_TWO_WAY_AUDIO_PUSHER_H
#define BACKCHANNEL_ISAPI_TWO_WAY_AUDIO_PUSHER_H

#include <memory>
#include <string>
#include "IBackchannelPusher.h"
#include "Common/Parser.h"
#include "Http/HttpRequestSplitter.h"
#include "Network/TcpClient.h"
#include "Poller/Timer.h"

namespace mediakit {

// Hikvision ISAPI two-way audio, asynchronous on one poller:
//   PUT <channel>/open → PUT <channel>/audioData (Content-Length: 0, then raw G.711 on the same
//   connection for the rest of the session) → PUT <channel>/close on a new connection.
// HTTP Digest (qop=auth) or Basic, from the challenge of the first request. A device holds one
// two-way session at a time: a second open is refused (403 deviceBusy), reported through onFailed
// with kDeviceBusyCode. Once open succeeded, close is always sent, whether the session ends,
// fails or is stopped, so the device does not stay busy until its own timeout.
class IsapiTwoWayAudioPusher : public IBackchannelPusher, public toolkit::TcpClient, public HttpRequestSplitter {
public:
    using Ptr = std::shared_ptr<IsapiTwoWayAudioPusher>;

    // SockException custom code of an onFailed caused by another two-way session on the device.
    static constexpr int kDeviceBusyCode = 403;

    IsapiTwoWayAudioPusher(const AudioBackchannelInfo &info, const toolkit::EventPoller::Ptr &poller = nullptr);
    ~IsapiTwoWayAudioPusher() override;

    void start() override;
    void attachSource(BackchannelMediaSource::Ptr source) override;
    void detachSource() override;
    void stopSession() override;

protected:
    void onConnect(const toolkit::SockException &ex) override;
    void onRecv(const toolkit::Buffer::Ptr &buf) override;
    void onError(const toolkit::SockException &err) override;
    void onManager() override {}

    ssize_t onRecvHeader(const char *data, size_t len) override;
    void onRecvContent(const char *data, size_t len) override;

private:
    enum class State { IDLE, OPENING, STARTING_DATA, STREAMING, CLOSING, DONE };

    void sendRequest(const std::string &action, bool stream);
    void onResponse();
    bool saveChallenge(const std::string &www_authenticate);
    std::string authorization(const std::string &method, const std::string &uri);

    void fail(const toolkit::SockException &ex);
    void sendClose();
    // Drops the connection and opens a new one; onConnect sends _action.
    void reconnect();
    void finish();
    void attachRingReader();
    void sendAudio(const BackchannelFrame::Ptr &frame);

private:
    std::string _url_error;
    std::string _host;
    uint16_t _port = 80;
    std::string _path;        // /ISAPI/System/TwoWayAudio/channels/<id>
    std::string _username;
    std::string _password;
    std::string _codec;

    State _state = State::IDLE;
    bool _opened = false;     // the device accepted open: close is owed
    std::string _action;      // open/audioData/close of the request in flight
    bool _retried = false;    // the request in flight already carries credentials
    bool _stream_request = false;
    Parser _response;
    size_t _content_left = 0;

    std::string _realm, _nonce, _opaque, _qop;
    bool _basic = false;
    unsigned _nc = 0;

    toolkit::Timer::Ptr _timer;
    std::shared_ptr<IsapiTwoWayAudioPusher> _self_hold; // keeps the object alive until close is answered

    BackchannelMediaSource::Ptr _media_source;
    BackchannelMediaSource::RingType::RingReader::Ptr _ring_reader;
    uint32_t _src_sample_rate = 0;
    uint8_t _src_channels = 0;
};

} // namespace mediakit

#endif // BACKCHANNEL_ISAPI_TWO_WAY_AUDIO_PUSHER_H
