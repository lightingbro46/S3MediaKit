#ifndef MEDIAKIT_BACKCHANNELPUSHER_H
#define MEDIAKIT_BACKCHANNELPUSHER_H

#include "Codec/G711Encoder.h"
#include "IAudioSource.h"
#include "Rtsp/RtpCodec.h"
#include "Rtcp/RtcpContext.h"
#include "Network/TcpClient.h"
#include "Poller/Timer.h"
#include "Rtsp/RtspSplitter.h"
#include "Common/Parser.h"
#include "Util/mini.h"
#include "BackchannelMediaSource.h"

#include <memory>
#include <string>
#include <functional>

namespace mediakit {

class BackchannelPusher : public toolkit::TcpClient, public mediakit::RtspSplitter, public toolkit::mINI {
public:
    using Ptr = std::shared_ptr<BackchannelPusher>;

    explicit BackchannelPusher(const std::string& strUrl, const toolkit::EventPoller::Ptr& poller = nullptr);
    ~BackchannelPusher() override;

    void start();
    void attachSource(BackchannelMediaSource::Ptr source);
    void detachSource();
    void stopSession();

    // Fired once, when the DESCRIBE/SETUP/PLAY handshake finishes successfully.
    std::function<void()> onReady;
    // Fired once, if the handshake (connect/DESCRIBE/SETUP/PLAY) fails.
    std::function<void(const toolkit::SockException&)> onFailed;
    // Fired if the session drops unexpectedly *after* onReady already fired.
    std::function<void(const toolkit::SockException&)> onShutdown;
    std::function<void()> onSourceFinished;

protected:
    void onConnect(const toolkit::SockException& ex) override;
    void onRecv(const toolkit::Buffer::Ptr& buf) override;
    void onError(const toolkit::SockException& err) override;
    void onManager() override {}

    void onWholeRtspPacket(mediakit::Parser& parser) override;
    void onRtpPacket(const char* data, size_t len) override;

private:
    enum class AuthScheme { None, Digest, Basic };

    void sendDescribe();
    void sendSetup();
    void sendPlay();
    void sendTeardown();

    void handleResDescribe(const mediakit::Parser& parser);
    void handleResSetup(const mediakit::Parser& parser);
    void handleResPlay(const mediakit::Parser& parser);

    void parseSdpAudioFormat(const std::string& sdp);

    void sendRtspRequest(const std::string& cmd, const std::string& url,
                         const std::initializer_list<std::string>& header);
    void sendRtspRequest(const std::string& cmd, const std::string& url,
                         const mediakit::StrCaseMap& headerConst = mediakit::StrCaseMap());

    std::string buildAuthorizationValue(const std::string& cmd, const std::string& url);
    bool handleAuthenticationFailure(const std::string& wwwAuthenticateValue);

    void startRtpSession();
    void sendRtpFrame(const BackchannelFrame::Ptr& frame);
    void setSocketFlags();

    void onResult_l(const toolkit::SockException& ex, bool handshake_done);
    void attachRingReader();

    void teardown();
    void reset();

private:
    std::string _url_parse_error;

    std::string _base_url;
    std::string _setup_url;
    std::string _device_host;
    uint16_t _rtsp_port = 0;

    int _cseq_send = 1;
    std::string _session_id;
    int _interleaved_channel = 0;
    int _rtcp_channel = 1;
    AuthScheme _auth_scheme = AuthScheme::None;
    std::string _realm;
    std::string _md5_nonce;
    bool _streaming = false;

    std::function<void(const mediakit::Parser&)> _on_res_func;
    toolkit::Timer::Ptr _handshake_timer;

    BackchannelMediaSource::Ptr _media_source;
    BackchannelMediaSource::RingType::RingReader::Ptr _ring_reader;

    uint32_t _src_sample_rate = 0;
    uint8_t _src_channels = 0;
    size_t _frame_samples = 160;
    mediakit::SdpTrack::Ptr _audio_track;
    std::unique_ptr<mediakit::RtpInfo> _rtp_info;
    mediakit::RtcpContextForSend _rtcp_context;
    toolkit::Timer::Ptr _rtp_timer;
    uint32_t _ssrc = 0;
    uint32_t _sent_frame_count = 0;
    toolkit::Ticker _rtcp_send_ticker;
};

} // namespace mediakit

#endif // MEDIAKIT_BACKCHANNELPUSHER_H