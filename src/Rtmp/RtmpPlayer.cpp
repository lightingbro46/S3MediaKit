#include "RtmpPlayer.h"
#include "Rtmp/utils.h"
#include "Util/util.h"
#include "Util/onceToken.h"
#include "Thread/ThreadPool.h"
#include "Common/config.h"
#include "Common/Parser.h"

#include "RtmpDemuxer.h"
#include "RtmpPlayerImp.h"

using namespace toolkit;
using namespace std;

namespace mediakit {

RtmpPlayer::RtmpPlayer(const EventPoller::Ptr &poller) : TcpClient(poller) {}

RtmpPlayer::~RtmpPlayer() {
    DebugL;
}

void RtmpPlayer::teardown() {
    if (alive()) {
        shutdown(SockException(Err_shutdown,"teardown"));
    }
    _app.clear();
    _stream_id.clear();
    _tc_url.clear();
    _beat_timer.reset();
    _play_timer.reset();
    _rtmp_recv_timer.reset();
    _seek_ms = 0;
    RtmpProtocol::reset();

    CLEAR_ARR(_fist_stamp);
    CLEAR_ARR(_now_stamp);

    _map_on_result.clear();
    _deque_on_status.clear();
}

void RtmpPlayer::play(const string &url)  {
    teardown();
    auto schema = findSubString(url.data(), nullptr, "://");
    auto host_url = findSubString(url.data(), "://", "/");
    _app = findSubString(url.data(), (host_url + "/").data(), "/");
    _stream_id = findSubString(url.data(), (host_url + "/" + _app + "/").data(), NULL);
    auto app_second = findSubString(_stream_id.data(), nullptr, "/");
    if (!app_second.empty() && app_second.find('?') == std::string::npos) {
        // _stream_id exists at multiple levels; it does not contain '?', indicating that the delimiter '/' is not part of the url parameter
        _app += "/" + app_second;
        _stream_id.erase(0, app_second.size() + 1);
    }
    _tc_url = schema + "://" + host_url + "/" + _app;
    if (_app.empty() || _stream_id.empty()) {
        onPlayResult_l(SockException(Err_other, "rtmp url illegal"), false);
        return;
    }
    DebugL << host_url << " " << _app << " " << _stream_id;

    uint16_t port = start_with(url, "rtmps") ? 443 : 1935;
    splitUrl(host_url, host_url, port);

    if (!(*this)[Client::kNetAdapter].empty()) {
        setNetAdapter((*this)[Client::kNetAdapter]);
    }

    weak_ptr<RtmpPlayer> weak_self = static_pointer_cast<RtmpPlayer>(shared_from_this());
    float play_timeout_sec = (*this)[Client::kTimeoutMS].as<int>() / 1000.0f;
    _play_timer.reset(new Timer(play_timeout_sec, [weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }
        strong_self->onPlayResult_l(SockException(Err_timeout, "play rtmp timeout"), false);
        return false;
    }, getPoller()));

    _metadata_got = false;
    startConnect(host_url, port, play_timeout_sec);
}

void RtmpPlayer::onError(const SockException &ex){
    // The timer _pPlayTimer is empty after the handshake is finished
    onPlayResult_l(ex, !_play_timer);
}

void RtmpPlayer::onPlayResult_l(const SockException &ex, bool handshake_done) {
    if (ex.getErrCode() == Err_shutdown) {
        // Active shutdown does not trigger a callback
        return;
    }

    WarnL << ex.getErrCode() << " " << ex;
    if (!handshake_done) {
        // Start playback stage
        _play_timer.reset();
        // Whether it is a performance test mode
        _benchmark_mode = (*this)[Client::kBenchmarkMode].as<int>();
        onPlayResult(ex);
    } else if (ex) {
        // Callback for abnormal disconnection after successful playback
        onShutdown(ex);
    } else {
        // Resume playback
        onResume();
    }

    if (!ex) {
        // After successful playback, restore the rtmp receive timeout timer
        _rtmp_recv_ticker.resetTime();
        auto timeout_ms = (*this)[Client::kMediaTimeoutMS].as<uint64_t>();
        weak_ptr<RtmpPlayer> weak_self = static_pointer_cast<RtmpPlayer>(shared_from_this());
        auto lam = [weak_self, timeout_ms]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            if (strong_self->_rtmp_recv_ticker.elapsedTime() > timeout_ms) {
                // Receive rtmp media data timeout
                SockException ex(Err_timeout, "receive rtmp timeout");
                strong_self->onPlayResult_l(ex, true);
                return false;
            }
            return true;
        };
        // Create an rtmp data receive timeout detection timer
        _rtmp_recv_timer = std::make_shared<Timer>(timeout_ms / 2000.0f, lam, getPoller());
    } else {
        shutdown(SockException(Err_shutdown,"teardown"));
    }
}

void RtmpPlayer::onConnect(const SockException &err) {
    if (err.getErrCode() != Err_success) {
        onPlayResult_l(err, false);
        return;
    }
    weak_ptr<RtmpPlayer> weak_self = static_pointer_cast<RtmpPlayer>(shared_from_this());
    startClientSession([weak_self]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->send_connect();
        }
    },_app.find("vod") != 0); //In actual tests, when vod on demand, use complex handshake fms without response: issue #2007
}

void RtmpPlayer::onRecv(const Buffer::Ptr &buf){
    try {
        if (_benchmark_mode && !_play_timer) {
            // In performance test mode, if the rtmp handshake is complete, the rtmp packet will no longer be parsed
            _rtmp_recv_ticker.resetTime();
            return;
        }
        onParseRtmp(buf->data(), buf->size());
    } catch (exception &e) {
        SockException ex(Err_other, e.what());
        // The timer _pPlayTimer is empty after the handshake is finished
        onPlayResult_l(ex, !_play_timer);
    }
}

void RtmpPlayer::pause(bool bPause) {
    send_pause(bPause);
}

void RtmpPlayer::speed(float speed) {
    //todo
}

void RtmpPlayer::send_connect() {
    AMFValue obj(AMF_OBJECT);
    obj.set("app", _app);
    obj.set("tcUrl", _tc_url);
    // No proxy used
    obj.set("fpad", false);
    // Refer to librtmp, what is the role?
    obj.set("capabilities", 15);
    // SUPPORT_VID_CLIENT_SEEK supports seek
    obj.set("videoFunction", 1);
    // Only supports aac
    obj.set("audioCodecs", (double) (0x0400));
    // Only supports H264
    obj.set("videoCodecs", (double) (0x0080));

    AMFValue fourCcList(AMF_STRICT_ARRAY);
    fourCcList.add("av01");
    fourCcList.add("vp09");
    fourCcList.add("hvc1");
    obj.set("fourCcList", fourCcList);

    sendInvoke("connect", obj);
    addOnResultCB([this](AMFDecoder &dec) {
        //TraceL << "connect result";
        dec.load<AMFValue>();
        auto val = dec.load<AMFValue>();
        auto level = val["level"].as_string();
        auto code = val["code"].as_string();
        if (level != "status") {
            throw std::runtime_error(StrPrinter << "connect failed:" << level << " " << code << endl);
        }
        send_createStream();
    });
}

void RtmpPlayer::send_createStream() {
    AMFValue obj(AMF_NULL);
    sendInvoke("createStream", obj);
    addOnResultCB([this](AMFDecoder &dec) {
        //TraceL << "createStream result";
        dec.load<AMFValue>();
        _stream_index = dec.load<int>();
        send_play();
    });
}

void RtmpPlayer::send_play() {
    AMFEncoder enc;
    enc << "play" << ++_send_req_id << nullptr << _stream_id << -2000;
    sendRequest(MSG_CMD, enc.data());
    auto fun = [](AMFValue &val) {
        //TraceL << "play onStatus";
        auto level = val["level"].as_string();
        auto code = val["code"].as_string();
        if (level != "status") {
            throw std::runtime_error(StrPrinter << "play failed:" << level << " " << code << endl);
        }
    };
    addOnStatusCB(fun);
    addOnStatusCB(fun);
}

void RtmpPlayer::send_pause(bool pause) {
    AMFEncoder enc;
    enc << "pause" << ++_send_req_id << nullptr << pause;
    sendRequest(MSG_CMD, enc.data());
    auto fun = [this, pause](AMFValue &val) {
        //TraceL << "pause onStatus";
        auto level = val["level"].as_string();
        auto code = val["code"].as_string();
        if (level != "status") {
            if (!pause) {
                throw std::runtime_error(StrPrinter << "pause failed to resume playback:" << level << " " << code << endl);
            }
        } else {
            _paused = pause;
            if (!pause) {
                onPlayResult_l(SockException(Err_success, "resum rtmp success"), true);
            } else {
                // Pause playback
                _rtmp_recv_timer.reset();
            }
        }
    };
    addOnStatusCB(fun);

    _beat_timer.reset();
    if (pause) {
        weak_ptr<RtmpPlayer> weak_self = static_pointer_cast<RtmpPlayer>(shared_from_this());
        _beat_timer.reset(new Timer((*this)[Client::kBeatIntervalMS].as<int>() / 1000.0f, [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            uint32_t timeStamp = (uint32_t)::time(NULL);
            strong_self->sendUserControl(CONTROL_PING_REQUEST, timeStamp);
            return true;
        }, getPoller()));
    }
}

void RtmpPlayer::onCmd_result(AMFDecoder &dec){
    auto req_id = dec.load<int>();
    auto it = _map_on_result.find(req_id);
    if (it != _map_on_result.end()) {
        it->second(dec);
        _map_on_result.erase(it);
    } else {
        WarnL << "unhandled _result";
    }
}

void RtmpPlayer::onCmd_onStatus(AMFDecoder &dec) {
    AMFValue val;
    while (true) {
        val = dec.load<AMFValue>();
        if (val.type() == AMF_OBJECT) {
            break;
        }
    }
    if (val.type() != AMF_OBJECT) {
        throw std::runtime_error("onStatus:the result object was not found");
    }

    if (_deque_on_status.size()) {
        _deque_on_status.front()(val);
        _deque_on_status.pop_front();
    } else {
        auto level = val["level"];
        auto code = val["code"].as_string();
        if (level.type() == AMF_STRING) {
            // warning should not be disconnected
            if (level.as_string() != "status" && level.as_string() != "warning") {
                throw std::runtime_error(StrPrinter << "onStatus failed:" << level.as_string() << " " << code << endl);
            }
        }
        //WarnL << "unhandled onStatus:" << code;
    }
}

void RtmpPlayer::onCmd_onMetaData(AMFDecoder &dec) {
    //TraceL;
    auto val = dec.load<AMFValue>();
    if (!onMetadata(val)) {
        throw std::runtime_error("onMetadata failed");
    }
    _metadata_got = true;
}

void RtmpPlayer::onStreamDry(uint32_t stream_index) {
    //TraceL << stream_index;
    onPlayResult_l(SockException(Err_other, "rtmp stream dry"), true);
}

void RtmpPlayer::onMediaData_l(RtmpPacket::Ptr chunk_data) {
    _rtmp_recv_ticker.resetTime();
    if (!_play_timer) {
        // The onPlayResult event has been triggered, directly trigger the onMediaData event
        onRtmpPacket(chunk_data);
        return;
    }

    if (chunk_data->isConfigFrame()) {
        // Input configuration frame to initialize each track
        onRtmpPacket(chunk_data);
    } else {
        // Trigger the onPlayResult event first, at this time the decoder can be initialized
        onPlayResult_l(SockException(Err_success, "play rtmp success"), false);
        // After triggering the onPlayResult event, input the frame data to the decoder
        onRtmpPacket(chunk_data);
    }
}

void RtmpPlayer::onRtmpChunk(RtmpPacket::Ptr packet) {
    auto &chunk_data = *packet;
    typedef void (RtmpPlayer::*rtmp_func_ptr)(AMFDecoder &dec);
    static unordered_map<string, rtmp_func_ptr> s_func_map;
    static onceToken token([]() {
        s_func_map.emplace("_error", &RtmpPlayer::onCmd_result);
        s_func_map.emplace("_result", &RtmpPlayer::onCmd_result);
        s_func_map.emplace("onStatus", &RtmpPlayer::onCmd_onStatus);
        s_func_map.emplace("onMetaData", &RtmpPlayer::onCmd_onMetaData);
    });

    switch (chunk_data.type_id) {
        case MSG_CMD:
        case MSG_CMD3:
        case MSG_DATA:
        case MSG_DATA3: {
            AMFDecoder dec(chunk_data.buffer, 0, (chunk_data.type_id == MSG_DATA3 || chunk_data.type_id == MSG_CMD3) ? 3 : 0);
            std::string type = dec.load<std::string>();
            auto it = s_func_map.find(type);
            if (it != s_func_map.end()) {
                auto fun = it->second;
                (this->*fun)(dec);
            } else {
                WarnL << "can not support cmd:" << type;
            }
            break;
        }

        case MSG_AUDIO:
        case MSG_VIDEO: {
            auto idx = chunk_data.type_id % 2;
            if (_now_stamp_ticker[idx].elapsedTime() > 500) {
                // Used to calculate the playback progress timeline
                _now_stamp[idx] = chunk_data.time_stamp;
            }
            if (!_metadata_got) {
                if (!onMetadata(TitleMeta().getMetadata())) {
                    throw std::runtime_error("onMetadata failed");
                }
                _metadata_got = true;
            }
            onMediaData_l(std::move(packet));
            break;
        }

        default: break;
    }
}

uint32_t RtmpPlayer::getProgressMilliSecond() const{
    uint32_t stamp[2] = {0, 0};
    for (auto i = 0; i < 2; i++) {
        stamp[i] = _now_stamp[i] - _fist_stamp[i];
    }
    return _seek_ms + MAX(stamp[0], stamp[1]);
}

void RtmpPlayer::seekToMilliSecond(uint32_t seekMS){
    if (_paused) {
        pause(false);
    }
    AMFEncoder enc;
    enc << "seek" << ++_send_req_id << nullptr << seekMS * 1.0;
    sendRequest(MSG_CMD, enc.data());
    addOnStatusCB([this, seekMS](AMFValue &val) {
        //TraceL << "seek result";
        _now_stamp_ticker[0].resetTime();
        _now_stamp_ticker[1].resetTime();
        int iTimeInc = seekMS - getProgressMilliSecond();
        for (auto i = 0; i < 2; i++) {
            _fist_stamp[i] = _now_stamp[i] + iTimeInc;
            _now_stamp[i] = _fist_stamp[i];
        }
        _seek_ms = seekMS;
    });
}

size_t RtmpPlayer::getRecvSpeed() {
    return TcpClient::getRecvSpeed();
}

size_t RtmpPlayer::getRecvTotalBytes() {
    return TcpClient::getRecvTotalBytes();
}

} /* namespace mediakit */
