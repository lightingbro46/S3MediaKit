#include "IsapiTwoWayAudioPusher.h"
#include "Backchannel/AudioResampler.h"
#include "Backchannel/IAudioSource.h"
#include "Codec/G711Encoder.h"
#include "Util/MD5.h"
#include "Util/base64.h"
#include "Util/logger.h"
#include "Util/util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <regex>

using namespace toolkit;

namespace mediakit {

namespace {
constexpr float REQUEST_TIMEOUT_SEC = 5.0f;

// "key=value" or key="value" of a WWW-Authenticate challenge.
std::string challengeParam(const std::string &challenge, const std::string &key) {
    std::regex re("(^|[\\s,])" + key + "=(\"([^\"]*)\"|([^,\\s]*))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(challenge, m, re)) {
        return "";
    }
    return m[3].matched ? m[3].str() : m[4].str();
}
} // namespace

constexpr int IsapiTwoWayAudioPusher::kDeviceBusyCode;

IsapiTwoWayAudioPusher::IsapiTwoWayAudioPusher(const AudioBackchannelInfo &info, const EventPoller::Ptr &poller)
    : TcpClient(poller), _username(info.username), _password(info.password), _codec(info.codec) {
    // http://host[:port]/path; credentials come in the info fields, not in the URL.
    static const std::regex url_re("^http://([^/:]+)(:(\\d+))?(/.*)$", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(info.url, m, url_re)) {
        _url_error = "illegal ISAPI two-way audio url: " + info.url;
        return;
    }
    _host = m[1].str();
    _port = m[3].matched ? (uint16_t)atoi(m[3].str().c_str()) : 80;
    _path = m[4].str();
    while (!_path.empty() && _path.back() == '/') {
        _path.pop_back();
    }
}

IsapiTwoWayAudioPusher::~IsapiTwoWayAudioPusher() {
    if (_opened && _state != State::DONE) {
        WarnL << "ISAPI two-way audio to " << _host << " destroyed without close; the device stays busy until its timeout";
    }
}

void IsapiTwoWayAudioPusher::start() {
    std::weak_ptr<IsapiTwoWayAudioPusher> weak_self = std::static_pointer_cast<IsapiTwoWayAudioPusher>(shared_from_this());
    getPoller()->async([weak_self]() {
        auto self = weak_self.lock();
        if (!self || self->_state != State::IDLE) {
            return;
        }
        if (!self->_url_error.empty()) {
            self->fail(SockException(Err_other, self->_url_error));
            return;
        }
        self->_state = State::OPENING;
        self->_action = "open";
        self->_retried = false;
        self->_stream_request = false;
        self->_timer = std::make_shared<Timer>(REQUEST_TIMEOUT_SEC, [weak_self]() {
            if (auto strong = weak_self.lock()) {
                strong->fail(SockException(Err_timeout, "ISAPI two-way audio open timeout"));
            }
            return false;
        }, self->getPoller());
        self->startConnect(self->_host, self->_port, REQUEST_TIMEOUT_SEC);
    });
}

void IsapiTwoWayAudioPusher::onConnect(const SockException &ex) {
    if (ex) {
        if (_state == State::CLOSING) {
            WarnL << "ISAPI two-way audio close to " << _host << " failed: " << ex.what();
            finish();
        } else {
            fail(ex);
        }
        return;
    }
    HttpRequestSplitter::reset();
    sendRequest(_action, _stream_request);
}

void IsapiTwoWayAudioPusher::onError(const SockException &err) {
    if (err.getErrCode() == Err_shutdown || _state == State::DONE) {
        return; // self-inflicted
    }
    if (_state == State::CLOSING) {
        WarnL << "ISAPI two-way audio close to " << _host << " failed: " << err.what();
        finish();
        return;
    }
    fail(err);
}

void IsapiTwoWayAudioPusher::onRecv(const Buffer::Ptr &buf) {
    if (_state == State::STREAMING || _state == State::DONE) {
        return; // nothing is expected back while streaming
    }
    input(buf->data(), buf->size());
}

ssize_t IsapiTwoWayAudioPusher::onRecvHeader(const char *data, size_t len) {
    if (len < 5 || strncmp(data, "HTTP/", 5) != 0) {
        return 0;
    }
    _response.parse(data, len);
    auto content_len = atoll(_response["Content-Length"].c_str());
    if (content_len > 0) {
        return content_len; // onRecvContent gets the whole body
    }
    onResponse();
    return 0;
}

void IsapiTwoWayAudioPusher::onRecvContent(const char *data, size_t len) {
    _response.setContent(std::string(data, len));
    onResponse();
}

void IsapiTwoWayAudioPusher::onResponse() {
    const int status = atoi(_response.status().c_str());
    const bool conn_close = strcasecmp(_response["Connection"].c_str(), "close") == 0;
    if (status == 401 && !_retried && saveChallenge(_response["WWW-Authenticate"])) {
        // Same request again, now with credentials.
        _retried = true;
        if (conn_close) {
            reconnect();
        } else {
            sendRequest(_action, _stream_request);
        }
        return;
    }

    switch (_state) {
    case State::OPENING:
        if (status / 100 != 2) {
            const bool busy = status == 403 && _response.content().find("deviceBusy") != std::string::npos;
            fail(SockException(Err_other, "ISAPI two-way audio open: HTTP " + _response.status() + (busy ? " deviceBusy" : ""),
                               busy ? kDeviceBusyCode : 0));
            return;
        }
        _opened = true;
        _state = State::STARTING_DATA;
        _action = "audioData";
        _retried = false;
        _stream_request = true;
        if (conn_close) {
            reconnect();
        } else {
            sendRequest(_action, _stream_request);
        }
        return;
    case State::STARTING_DATA:
        if (status / 100 != 2) {
            fail(SockException(Err_other, "ISAPI two-way audio audioData: HTTP " + _response.status()));
            return;
        }
        _timer.reset();
        _state = State::STREAMING;
        InfoL << "ISAPI two-way audio to " << _host << _path << " started (" << _codec << ")";
        attachRingReader();
        if (onReady) {
            onReady();
        }
        return;
    case State::CLOSING:
        if (status / 100 != 2) {
            WarnL << "ISAPI two-way audio close to " << _host << ": HTTP " << _response.status();
        }
        finish();
        return;
    default:
        return;
    }
}

bool IsapiTwoWayAudioPusher::saveChallenge(const std::string &www_authenticate) {
    if (_username.empty()) {
        return false;
    }
    if (strncasecmp(www_authenticate.c_str(), "Basic", 5) == 0) {
        _basic = true;
        return true;
    }
    if (strncasecmp(www_authenticate.c_str(), "Digest", 6) != 0) {
        return false;
    }
    _basic = false;
    _realm = challengeParam(www_authenticate, "realm");
    _nonce = challengeParam(www_authenticate, "nonce");
    _opaque = challengeParam(www_authenticate, "opaque");
    // "auth" or "auth,auth-int": only auth is used.
    _qop = challengeParam(www_authenticate, "qop").find("auth") != std::string::npos ? "auth" : "";
    _nc = 0;
    return !_nonce.empty();
}

std::string IsapiTwoWayAudioPusher::authorization(const std::string &method, const std::string &uri) {
    if (_basic) {
        return "Basic " + encodeBase64(_username + ":" + _password);
    }
    const std::string ha1 = MD5(_username + ":" + _realm + ":" + _password).hexdigest();
    const std::string ha2 = MD5(method + ":" + uri).hexdigest();
    _StrPrinter printer;
    printer << "Digest username=\"" << _username << "\", realm=\"" << _realm << "\", nonce=\"" << _nonce
            << "\", uri=\"" << uri << "\"";
    if (_qop.empty()) {
        printer << ", response=\"" << MD5(ha1 + ":" + _nonce + ":" + ha2).hexdigest() << "\"";
    } else {
        char nc[9];
        snprintf(nc, sizeof(nc), "%08x", ++_nc);
        const std::string cnonce = MD5(makeRandStr(16)).hexdigest().substr(0, 16);
        printer << ", qop=" << _qop << ", nc=" << nc << ", cnonce=\"" << cnonce << "\", response=\""
                << MD5(ha1 + ":" + _nonce + ":" + nc + ":" + cnonce + ":" + _qop + ":" + ha2).hexdigest() << "\"";
    }
    if (!_opaque.empty()) {
        printer << ", opaque=\"" << _opaque << "\"";
    }
    return printer;
}

void IsapiTwoWayAudioPusher::sendRequest(const std::string &action, bool stream) {
    const std::string uri = _path + "/" + action;
    _StrPrinter printer;
    printer << "PUT " << uri << " HTTP/1.1\r\n";
    printer << "Host: " << _host << ":" << _port << "\r\n";
    if (_basic || !_nonce.empty()) {
        printer << "Authorization: " << authorization("PUT", uri) << "\r\n";
    }
    if (stream) {
        // Content-Length 0 ends the request for the HTTP layer; the device answers and then reads
        // raw audio from the connection until it closes.
        printer << "Content-Type: application/octet-stream\r\n";
    }
    printer << "Connection: keep-alive\r\n";
    printer << "Content-Length: 0\r\n\r\n";
    SockSender::send(printer);
}

void IsapiTwoWayAudioPusher::fail(const SockException &ex) {
    if (_state == State::CLOSING || _state == State::DONE) {
        return;
    }
    const bool was_streaming = _state == State::STREAMING;
    _timer.reset();
    WarnL << "ISAPI two-way audio to " << _host << _path << " failed: " << ex.what();
    if (_opened) {
        sendClose();
    } else {
        finish();
    }
    // Callbacks last: they may drop the owner's reference (close holds its own).
    if (was_streaming) {
        if (onShutdown) {
            onShutdown(ex);
        }
    } else if (onFailed) {
        onFailed(ex);
    }
}

void IsapiTwoWayAudioPusher::sendClose() {
    _state = State::CLOSING;
    _ring_reader.reset();
    _self_hold = std::static_pointer_cast<IsapiTwoWayAudioPusher>(shared_from_this());
    std::weak_ptr<IsapiTwoWayAudioPusher> weak_self = _self_hold;
    _timer = std::make_shared<Timer>(REQUEST_TIMEOUT_SEC, [weak_self]() {
        if (auto self = weak_self.lock()) {
            WarnL << "ISAPI two-way audio close to " << self->_host << " timed out";
            self->finish();
        }
        return false;
    }, getPoller());
    // The audioData connection only carries audio: close goes on a new one.
    _action = "close";
    _retried = false;
    _stream_request = false;
    reconnect();
}

void IsapiTwoWayAudioPusher::reconnect() {
    shutdown(SockException(Err_shutdown, "reconnect"));
    // Connect from a later task: this may run inside the read callback of the socket being replaced.
    std::weak_ptr<IsapiTwoWayAudioPusher> weak_self = std::static_pointer_cast<IsapiTwoWayAudioPusher>(shared_from_this());
    getPoller()->async([weak_self]() {
        auto self = weak_self.lock();
        if (self && self->_state != State::DONE) {
            self->startConnect(self->_host, self->_port, REQUEST_TIMEOUT_SEC);
        }
    }, false);
}

void IsapiTwoWayAudioPusher::finish() {
    if (_state == State::CLOSING) {
        InfoL << "ISAPI two-way audio to " << _host << _path << " closed";
    }
    _state = State::DONE;
    _opened = false;
    _timer.reset();
    _ring_reader.reset();
    _media_source.reset();
    shutdown(SockException(Err_shutdown, "two-way audio done"));
    if (_self_hold) {
        // Drop the hold from a later task: this call may be running inside one of our own callbacks.
        auto hold = std::move(_self_hold);
        getPoller()->async([hold]() {}, false);
    }
}

void IsapiTwoWayAudioPusher::stopSession() {
    auto self = std::static_pointer_cast<IsapiTwoWayAudioPusher>(shared_from_this());
    getPoller()->async([self]() {
        switch (self->_state) {
        case State::CLOSING:
        case State::DONE:
            return;
        case State::IDLE:
            self->_state = State::DONE;
            return;
        default:
            break;
        }
        self->_timer.reset();
        if (self->_opened) {
            self->sendClose();
        } else {
            self->finish();
        }
    });
}

void IsapiTwoWayAudioPusher::attachSource(BackchannelMediaSource::Ptr source) {
    std::weak_ptr<IsapiTwoWayAudioPusher> weak_self = std::static_pointer_cast<IsapiTwoWayAudioPusher>(shared_from_this());
    getPoller()->async([weak_self, source]() {
        auto self = weak_self.lock();
        if (!self || self->_state == State::CLOSING || self->_state == State::DONE) {
            return;
        }
        self->_ring_reader.reset();
        self->_media_source = source;
        if (auto meta = dynamic_cast<IAudioSource *>(source.get())) {
            self->_src_sample_rate = meta->sampleRate();
            self->_src_channels = meta->channels();
        } else {
            WarnL << "BackchannelMediaSource attached without an IAudioSource facet; nothing will be sent";
            self->_src_sample_rate = 0;
            self->_src_channels = 0;
        }
        self->attachRingReader(); // no-op until audioData is accepted
    });
}

void IsapiTwoWayAudioPusher::detachSource() {
    std::weak_ptr<IsapiTwoWayAudioPusher> weak_self = std::static_pointer_cast<IsapiTwoWayAudioPusher>(shared_from_this());
    getPoller()->async([weak_self]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        self->_ring_reader.reset();
        self->_media_source.reset();
        self->_src_sample_rate = 0;
        self->_src_channels = 0;
    });
}

void IsapiTwoWayAudioPusher::attachRingReader() {
    if (_ring_reader || !_media_source || _state != State::STREAMING) {
        return;
    }
    std::weak_ptr<IsapiTwoWayAudioPusher> weak_self = std::static_pointer_cast<IsapiTwoWayAudioPusher>(shared_from_this());
    _ring_reader = _media_source->getRing()->attach(getPoller());
    _ring_reader->setReadCB([weak_self](const BackchannelFrame::Ptr &frame) {
        if (auto self = weak_self.lock()) {
            self->sendAudio(frame);
        }
    });
}

void IsapiTwoWayAudioPusher::sendAudio(const BackchannelFrame::Ptr &frame) {
    if (_state != State::STREAMING || !frame || _src_sample_rate == 0) {
        return;
    }
    const int16_t *pcm = reinterpret_cast<const int16_t *>(frame->data());
    size_t samples = frame->size() / sizeof(int16_t);
    std::vector<int16_t> resampled;
    if (_src_sample_rate != 8000 || _src_channels != 1) {
        resampled = resampleLinear(pcm, samples, _src_sample_rate, _src_channels, 8000, 1);
        pcm = resampled.data();
        samples = resampled.size();
    }
    if (!samples) {
        return;
    }
    auto buf = BufferRaw::create();
    buf->setCapacity(samples + 1);
    buf->setSize(samples);
    if (_codec == "PCMA") {
        G711Encoder::pcm16ToAlaw(pcm, samples, reinterpret_cast<uint8_t *>(buf->data()));
    } else {
        G711Encoder::pcm16ToUlaw(pcm, samples, reinterpret_cast<uint8_t *>(buf->data()));
    }
    send(std::move(buf));
}

} // namespace mediakit
