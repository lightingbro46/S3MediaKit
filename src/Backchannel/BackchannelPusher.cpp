#include "BackchannelPusher.h"
#include "Backchannel/AudioResampler.h"

#include "Network/Buffer.h"
#include "Util/logger.h"
#include "Util/MD5.h"
#include "Util/base64.h"
#include "Common/macros.h"
#include "Common/config.h"

#include <random>
#include <regex>
#include <memory>
#include <cstdio>

using namespace toolkit;

namespace mediakit {

namespace {
constexpr float RTP_INTERVAL_SEC = 0.02f;
constexpr uint64_t RTCP_INTERVAL_MS = 5000;
constexpr float HANDSHAKE_TIMEOUT_SEC = 5.0f;
constexpr const char* CONTROL_SUFFIX = "/audioback";

using EncodeFn = void(*)(const int16_t*, size_t, uint8_t*);
static const std::unordered_map<std::string, EncodeFn> kPcmEncoders = {
    { "PCMU", &G711Encoder::pcm16ToUlaw },
    { "PCMA", &G711Encoder::pcm16ToAlaw }
};
} // namespace

BackchannelPusher::BackchannelPusher(const std::string& strUrl, const EventPoller::Ptr& poller) : TcpClient(poller) {
    RtspUrl url;
    try {
        url.parse(strUrl);
    } catch (std::exception& ex) {
        _url_parse_error = ex.what();
        return;
    }

    if (url._user.size()) {
        (*this)[Client::kRtspUser] = url._user;
    }
    if (url._passwd.size()) {
        (*this)[Client::kRtspPwd] = url._passwd;
        (*this)[Client::kRtspPwdIsMD5] = false; // Currently, kRtspPwdIsMD5 is always false
    }

    _device_host = url._host;
    _rtsp_port = url._port;

    _base_url = url._url;
    _setup_url = _base_url + CONTROL_SUFFIX;
}

BackchannelPusher::~BackchannelPusher() {
    teardown();
}

void BackchannelPusher::start() {
    teardown(); // idempotent: safe even if start() is called again to restart a session

    if (!_url_parse_error.empty()) {
        onResult_l(SockException(Err_other, "illegal rtsp url: " + _url_parse_error), false);
        return;
    }

    std::weak_ptr<BackchannelPusher> weakSelf = std::static_pointer_cast<BackchannelPusher>(shared_from_this());
    _handshake_timer = std::make_shared<Timer>(HANDSHAKE_TIMEOUT_SEC, [weakSelf]() {
        auto self = weakSelf.lock();
        if (!self) return false;
        self->onResult_l(SockException(Err_timeout, "connect/handshake timeout"), false);
        return false;
    }, getPoller());

    startConnect(_device_host, _rtsp_port, HANDSHAKE_TIMEOUT_SEC);
}

void BackchannelPusher::onConnect(const SockException& ex) {
    if (ex) {
        onResult_l(ex, false);
        return;
    }
    InfoL << "Connected to device " << _device_host << ":" << _rtsp_port;
    sendDescribe();
}

void BackchannelPusher::onError(const SockException& err) {
    _rtp_timer.reset();
    // Once the handshake timer has been cleared (see onResult_l), _streaming tells us
    // whether the drop happened mid-stream or during the initial handshake.
    onResult_l(err, _streaming);
}

void BackchannelPusher::onRecv(const Buffer::Ptr& buf) {
    try {
        input(buf->data(), buf->size());
    } catch (std::exception& e) {
        onResult_l(SockException(Err_other, e.what()), _streaming);
    }
}

void BackchannelPusher::onWholeRtspPacket(Parser& parser) {
    decltype(_on_res_func) func;
    _on_res_func.swap(func);
    if (func) {
        func(parser);
    }
    parser.clear();
}

void BackchannelPusher::onRtpPacket(const char* data, size_t len) {
    if (len < RtpPacket::kRtpTcpHeaderSize) return;
    uint8_t channel = (uint8_t)data[1];
    if ((int)channel != _rtcp_channel) return;
    // RTCP Receiver Report(s) from the device; nothing to act on yet, but keep the parse
    // path so future round-trip/loss monitoring can hook in here.
    auto rtcp_arr = RtcpHeader::loadFromBytes((char*)data + RtpPacket::kRtpTcpHeaderSize, len - RtpPacket::kRtpTcpHeaderSize);
    for (auto& rtcp : rtcp_arr) {
        TraceL << "Received RTCP packet type=" << (int)rtcp->pt << " on backchannel channel " << _rtcp_channel;
    }
}

void BackchannelPusher::onResult_l(const SockException& ex, bool handshake_done) {
    if (ex.getErrCode() == Err_shutdown) {
        // Self-inflicted via stopSession()/teardown(); the caller already knows.
        return;
    }
    if (!handshake_done) {
        _handshake_timer.reset();
        if (ex) {
            if (onFailed) onFailed(ex);
        } else {
            _streaming = true;
            if (onReady) onReady();
        }
    } else if (onShutdown) {
        onShutdown(ex);
    }
    if (ex) {
        sendTeardown();
    }
}

bool BackchannelPusher::handleAuthenticationFailure(const std::string& paramsStr) {
    if (_auth_scheme != AuthScheme::None && paramsStr.find("stale=true") == std::string::npos) {
        return false;
    }

    char* realm = new char[paramsStr.size() + 1];
    char* nonce = new char[paramsStr.size() + 1];
    std::unique_ptr<char[]> realmGuard(realm), nonceGuard(nonce);

    if (sscanf(paramsStr.data(), "Digest realm=\"%[^\"]\", nonce=\"%[^\"]\"", realm, nonce) == 2) {
        _realm = realm;
        _md5_nonce = nonce;
        _auth_scheme = AuthScheme::Digest;
        return true;
    }
    if (sscanf(paramsStr.data(), "Basic realm=\"%[^\"]\"", realm) == 1) {
        _realm = realm;
        _auth_scheme = AuthScheme::Basic;
        return true;
    }
    return false;
}

std::string BackchannelPusher::buildAuthorizationValue(const std::string& cmd, const std::string& url) {
    if (_auth_scheme == AuthScheme::Basic) {
        if (!(*this)[Client::kRtspPwdIsMD5].as<bool>()) {
            return StrPrinter << "Basic " << encodeBase64((*this)[Client::kRtspUser] + ":" + (*this)[Client::kRtspPwd]);
        }
        return "";
    }

    std::string ha1 = (*this)[Client::kRtspPwdIsMD5].as<bool>() ? (*this)[Client::kRtspPwd]
                    : MD5((*this)[Client::kRtspUser] + ":" + _realm + ":" + (*this)[Client::kRtspPwd]).hexdigest();
    std::string ha2 = MD5(cmd + ":" + url).hexdigest();
    std::string response = MD5(ha1 + ":" + _md5_nonce + ":" + ha2).hexdigest();

    _StrPrinter printer;
    printer << "Digest ";
    printer << "username=\"" << (*this)[Client::kRtspUser] << "\", ";
    printer << "realm=\"" << _realm << "\", ";
    printer << "nonce=\"" << _md5_nonce << "\", ";
    printer << "uri=\"" << url << "\", ";
    printer << "response=\"" << response << "\"";
    return printer;
}

void BackchannelPusher::sendRtspRequest(const std::string& cmd, const std::string& url,
                                         const std::initializer_list<std::string>& header) {
    std::string key;
    StrCaseMap header_map;
    int i = 0;
    for (auto& val : header) {
        if (++i % 2 == 0) {
            header_map.emplace(key, val);
        } else {
            key = val;
        }
    }
    sendRtspRequest(cmd, url, header_map);
}

void BackchannelPusher::sendRtspRequest(const std::string& cmd, const std::string& url, const StrCaseMap& headerConst) {
    auto header = headerConst;
    header.emplace("CSeq", StrPrinter << _cseq_send++);
    header.emplace("User-Agent", kServerName);

    if (!_session_id.empty()) {
        header.emplace("Session", _session_id);
    }
    if (_auth_scheme != AuthScheme::None) {
        header.emplace("Authorization", buildAuthorizationValue(cmd, url));
    }

    _StrPrinter printer;
    printer << cmd << " " << url << " RTSP/1.0\r\n";
    for (auto& pr : header) {
        printer << pr.first << ": " << pr.second << "\r\n";
    }
    printer << "\r\n";
    SockSender::send(std::move(printer));
}

void BackchannelPusher::sendDescribe() {
    _on_res_func = [this](const Parser& parser) { handleResDescribe(parser); };
    sendRtspRequest("DESCRIBE", _base_url,
                     {"Accept", "application/sdp", "Require", "www.onvif.org/ver20/backchannel"});
}

void BackchannelPusher::handleResDescribe(const Parser& parser) {
    if (parser.status() == "401" && handleAuthenticationFailure(parser["WWW-Authenticate"])) {
        sendDescribe();
        return;
    }
    if (parser.status() != "200") {
        throw std::runtime_error("DESCRIBE failed, status=" + parser.status());
    }
    parseSdpAudioFormat(parser.content());
    sendSetup();
}

void BackchannelPusher::sendSetup() {
    _on_res_func = [this](const Parser& parser) { handleResSetup(parser); };
    sendRtspRequest("SETUP", _setup_url, {"Transport", "RTP/AVP/TCP;unicast;interleaved=0-1"});
}

void BackchannelPusher::handleResSetup(const Parser& parser) {
    if (parser.status() == "401" && handleAuthenticationFailure(parser["WWW-Authenticate"])) {
        sendSetup();
        return;
    }
    if (parser.status() != "200") {
        throw std::runtime_error("SETUP failed, status=" + parser.status());
    }

    if (!parser["Session"].empty()) {
        _session_id = parser["Session"];
        auto semiPos = _session_id.find(';');
        if (semiPos != std::string::npos) _session_id = _session_id.substr(0, semiPos);
    }

    const std::string& transport = parser["Transport"];
    static const std::regex interleavedRe("interleaved=(\\d+)-(\\d+)");
    std::smatch m;
    if (std::regex_search(transport, m, interleavedRe)) {
        _interleaved_channel = std::stoi(m[1].str());
        _rtcp_channel = std::stoi(m[2].str());
    }

    sendPlay();
}

void BackchannelPusher::sendPlay() {
    _on_res_func = [this](const Parser& parser) { handleResPlay(parser); };
    sendRtspRequest("PLAY", _base_url, {"Require", "www.onvif.org/ver20/backchannel"});
}

void BackchannelPusher::handleResPlay(const Parser& parser) {
    if (parser.status() == "401" && handleAuthenticationFailure(parser["WWW-Authenticate"])) {
        sendPlay();
        return;
    }
    if (parser.status() != "200") {
        throw std::runtime_error("PLAY failed, status=" + parser.status());
    }

    enableRecvRtp(true);
    startRtpSession();
    setSocketFlags();

    InfoL << "RTSP PLAY succeeded. Starting backchannel to " << _device_host << ", channel=" << _interleaved_channel;
    onResult_l(SockException(Err_success, "success"), false);
}

void BackchannelPusher::sendTeardown() {
    if (alive()) {
        if (!_session_id.empty()) {
            sendRtspRequest("TEARDOWN", _base_url);
        }
        shutdown(SockException(Err_shutdown, "teardown"));
    }
}

void BackchannelPusher::parseSdpAudioFormat(const std::string& sdp) {
    SdpParser sdpParser(sdp);

    auto isSupportedCodec = [](const std::string& codec) {
        return kPcmEncoders.find(codec) != kPcmEncoders.end();
    };

    _audio_track = nullptr;

    for (const auto& track : sdpParser.getAvailableTrack()) {
        if (track &&
            track->_type == TrackAudio &&
            track->_samplerate > 0 &&
            track->_channel > 0 &&
            track->_pt >= 0 &&
            isSupportedCodec(track->_codec)) {

            InfoL << "Found supported audio track:"
                  << " codec=" << track->_codec
                  << ", payloadType=" << track->_pt
                  << ", sampleRate=" << track->_samplerate
                  << ", channels=" << track->_channel;

            _audio_track = track;
            break;
        }
    }
}

void BackchannelPusher::attachSource(BackchannelMediaSource::Ptr source) {
    std::weak_ptr<BackchannelPusher> weakSelf = std::static_pointer_cast<BackchannelPusher>(shared_from_this());
    async([weakSelf, source]() {
        auto self = weakSelf.lock();
        if (!self) return;

        self->_media_source = source;

        if (auto meta = dynamic_cast<IAudioSource*>(source.get())) {
            self->_src_sample_rate = meta->sampleRate();
            self->_src_channels = meta->channels();
        } else {
            WarnL << "BackchannelMediaSource attached without an IAudioSource facet - "
                     "resample target metadata unavailable, RTP send will stay off "
                     "until a matching source is attached";
        }

        self->attachRingReader(); // no-op if PLAY hasn't succeeded yet
    });
}

void BackchannelPusher::attachRingReader() {
    if (_ring_reader || !_media_source || !_audio_track) {
        // either already attached, or one of the two prerequisites
        // (source / negotiated track) hasn't arrived yet - whichever of
        // attachSource()/prepareRtpSession() runs second is the one that
        // actually completes this.
        return;
    }

    std::weak_ptr<BackchannelPusher> weakSelf = std::static_pointer_cast<BackchannelPusher>(shared_from_this());
    _ring_reader = _media_source->getRing()->attach(getPoller());
    _ring_reader->setReadCB([weakSelf](const BackchannelFrame::Ptr& frame) {
        auto self = weakSelf.lock();
        if (!self) return;
        self->sendRtpFrame(frame);
    });
}

void BackchannelPusher::detachSource() {
    std::weak_ptr<BackchannelPusher> weakSelf = std::static_pointer_cast<BackchannelPusher>(shared_from_this());
    async([weakSelf]() {
        auto self = weakSelf.lock();
        if (!self) return;
        self->_ring_reader.reset();
        self->_media_source.reset();
        self->_src_sample_rate = 0;
        self->_src_channels = 0;
    });
}

void BackchannelPusher::startRtpSession() {
    if (!_audio_track) {
        throw std::runtime_error("No supported audio track found. Supported codecs: PCMU, PCMA");
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    _ssrc = std::uniform_int_distribution<uint32_t>(0, 0xFFFFFFFFu)(gen);
    uint32_t sampleRate = _audio_track->_samplerate;
    uint32_t payloadType = _audio_track->_pt;

    _frame_samples = (size_t)(sampleRate * 20 / 1000);
    if (_frame_samples == 0) {
        // This case should never happen
        WarnL << "Unexpected sample rate (" << sampleRate << "); falling back to 160 samples @ 8kHz";
        _frame_samples = 160;
        sampleRate = 8000;
    }

    _rtp_info.reset(new RtpInfo(_ssrc, /*mtu_size=*/1500, sampleRate, (uint8_t)payloadType, (uint8_t)_interleaved_channel, /*track_index=*/0));
    _sent_frame_count = 0;
    _rtcp_send_ticker.resetTime();
    
    attachRingReader();
}

void BackchannelPusher::sendRtpFrame(const BackchannelFrame::Ptr& frame) {
    if (!_audio_track || !frame || _src_sample_rate == 0) {
        return;
    }

    const int16_t* pcm = reinterpret_cast<const int16_t*>(frame->data());
    size_t samples = frame->size() / sizeof(int16_t);
    std::vector<int16_t> resampledFrame;

    if (static_cast<int>(_src_sample_rate) != _audio_track->_samplerate || static_cast<int>(_src_channels) != _audio_track->_channel) {
        resampledFrame = resampleLinear(
            pcm,
            samples,
            _src_sample_rate,
            _src_channels,
            _audio_track->_samplerate,
            _audio_track->_channel);
        pcm = resampledFrame.data();
        samples = resampledFrame.size();
    }

    std::vector<int16_t> finalPcm(_frame_samples, 0);
    std::memcpy(finalPcm.data(), pcm, std::min(samples, _frame_samples) * sizeof(int16_t));

    auto it = kPcmEncoders.find(_audio_track->_codec);
    if (it == kPcmEncoders.end()) {
        WarnL << "Unsupported audio codec: " << _audio_track->_codec;
        return;
    }

    std::vector<uint8_t> encodedFrame(_frame_samples);
    it->second(finalPcm.data(), _frame_samples, encodedFrame.data());

    bool isFirstPacket = (_sent_frame_count == 0);
    uint64_t stampMs = static_cast<uint64_t>(_sent_frame_count) * 20;

    auto rtp = _rtp_info->makeRtp(TrackAudio, encodedFrame.data(), _frame_samples, isFirstPacket, stampMs);
    send(rtp);
    ++_sent_frame_count;

    _rtcp_context.onRtp(
        rtp->getSeq(),
        rtp->getStamp(),
        rtp->ntp_stamp,
        rtp->sample_rate,
        rtp->size() - RtpPacket::kRtpTcpHeaderSize);

    if (_rtcp_send_ticker.elapsedTime() >= RTCP_INTERVAL_MS) {
        auto sr = _rtcp_context.createRtcpSR(_ssrc);
        send(makeRtpOverTcpPrefix((uint16_t)sr->size(), (uint8_t)_rtcp_channel));
        send(std::move(sr));
        _rtcp_send_ticker.resetTime();
    }
}

void BackchannelPusher::setSocketFlags() {
    GET_CONFIG(int, merge_write_ms, General::kMergeWriteMS);
    if (merge_write_ms > 0) {
        setSendFlags(SOCKET_DEFAULE_FLAGS | FLAG_MORE);
        SockUtil::setNoDelay(getSock()->rawFD(), false);
    }
}

void BackchannelPusher::stopSession() {
    if (_streaming && _rtp_info) {
        auto bye = RtcpBye::create({_ssrc}, "backchannel session stopped");
        auto buf = RtcpHeader::toBuffer(bye);
        send(makeRtpOverTcpPrefix((uint16_t)buf->size(), (uint8_t)_rtcp_channel));
        send(std::move(buf));
    }
    teardown();
}

void BackchannelPusher::teardown() {
    sendTeardown();
    reset();
}

void BackchannelPusher::reset() {
    _rtp_timer.reset();
    _handshake_timer.reset();
    _rtp_info.reset();
    _audio_track.reset();
    _on_res_func = nullptr;

    _session_id.clear();
    _realm.clear();
    _md5_nonce.clear();
    _auth_scheme = AuthScheme::None;

    _cseq_send = 1;
    _interleaved_channel = 0;
    _rtcp_channel = 1;
    _sent_frame_count = 0;
    _streaming = false;
}

} // namespace mediakit