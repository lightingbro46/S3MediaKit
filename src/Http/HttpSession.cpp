#include <stdio.h>
#include <sys/stat.h>
#include <algorithm>
#include "Common/config.h"
#include "Common/strCoding.h"
#include "HttpSession.h"
#include "HttpConst.h"
#include "Util/base64.h"
#include "Util/SHA1.h"
#include "Util/MD5.h"
#if defined(ENABLE_FFMPEG)
#include "Common/MultiMediaSourceMuxer.h"
#include "Transcode/OverlayPrivacyUtils.h"
#include "Transcode/TranscodeProcessor.h"
#endif // ENABLE_FFMPEG

using namespace std;
using namespace toolkit;

namespace mediakit {

HttpSession::HttpSession(const Socket::Ptr &pSock) : Session(pSock) {
    // Set default parameters
    setMaxReqSize(0);
    setTimeoutSec(0);
}

void HttpSession::onHttpRequest_HEAD() {
    // Temporarily return 200 OK for all, because HTTP GET has on-demand generation stream operations, so it cannot return according to the HTTP GET process
    // If you return 404 directly, it will also cause the on-demand generation stream logic to fail, so HTTP HEAD is only valid for static files or existing resources
    // Not applicable to live streaming scenarios that generate streams on demand
    sendResponse(200, false);
}

void HttpSession::onHttpRequest_OPTIONS() {
    KeyValue header;
    header.emplace("Allow", "GET, POST, HEAD, OPTIONS");
    GET_CONFIG(bool, allow_cross_domains, Http::kAllowCrossDomains);
    if (allow_cross_domains) {
        header.emplace("Access-Control-Allow-Origin", "*");
        header.emplace("Access-Control-Allow-Headers", "*");
        header.emplace("Access-Control-Allow-Methods", "GET, POST, HEAD, OPTIONS");
    }
    header.emplace("Access-Control-Allow-Credentials", "true");
    header.emplace("Access-Control-Request-Methods", "GET, POST, OPTIONS");
    header.emplace("Access-Control-Request-Headers", "Accept,Accept-Language,Content-Language,Content-Type");
    sendResponse(200, true, nullptr, header);
}

ssize_t HttpSession::onRecvHeader(const char *header, size_t len) {
    using func_type = void (HttpSession::*)();
    static unordered_map<string, func_type> s_func_map;
    static onceToken token([]() {
        s_func_map.emplace("GET", &HttpSession::onHttpRequest_GET);
        s_func_map.emplace("POST", &HttpSession::onHttpRequest_POST);
        // DELETE command is used for whip/whep, only used to trigger http api
        s_func_map.emplace("DELETE", &HttpSession::onHttpRequest_POST);
        s_func_map.emplace("HEAD", &HttpSession::onHttpRequest_HEAD);
        s_func_map.emplace("OPTIONS", &HttpSession::onHttpRequest_OPTIONS);
    });

    _parser.parse(header, len);
    CHECK(_parser.url()[0] == '/');
    _origin = _parser["Origin"];

    urlDecode(_parser);
    auto &cmd = _parser.method();
    auto it = s_func_map.find(cmd);
    if (it == s_func_map.end()) {
        WarnP(this) << "Http method not supported: " << cmd;
        sendResponse(405, true);
        return 0;
    }

    size_t content_len;
    auto &content_len_str = _parser["Content-Length"];
    if (content_len_str.empty()) {
        if (it->first == "POST") {
            // Http post does not specify length, we consider it to be an indefinite length body
            WarnL << "Received http post request without content-length, consider it to be unlimited length";
            content_len = SIZE_MAX;
        } else {
            content_len = 0;
        }
    } else {
        // Length has been specified
        content_len = atoll(content_len_str.data());
    }

    if (content_len == 0) {
        // // No body case, trigger callback directly ////
        (this->*(it->second))();
        _parser.clear();
        // If _on_recv_body is set, it means that the body will be processed later
        return _on_recv_body ? -1 : 0;
    }

    if (content_len > _max_req_size) {
        // // Indefinite length body or oversized body ////
        if (content_len != SIZE_MAX) {
            WarnL << "Http body size is too huge: " << content_len << " > " << _max_req_size
                  << ", please set " << Http::kMaxReqSize << " in config.ini file.";
        }

        size_t received = 0;
        auto parser = std::move(_parser);
        _on_recv_body = [this, parser, received, content_len](const char *data, size_t len) mutable {
            received += len;
            onRecvUnlimitedContent(parser, data, len, content_len, received);
            if (received < content_len) {
                // Not yet received
                return true;
            }

            // Received full
            setContentLen(0);
            return false;
        };
        // Declare that the following is all body; Http body is buffered in this object, not saved through HttpRequestSplitter
        return -1;
    }

    // // Body size is explicitly specified and less than the maximum value ////
    _on_recv_body = [this, it](const char *data, size_t len) mutable {
        // Body collection complete
        _parser.setContent(std::string(data, len));
        (this->*(it->second))();
        _parser.clear();

        // _on_recv_body is cleared
        return false;
    };

    // Declare the body length, cache it through HttpRequestSplitter and then callback to _on_recv_body at once
    return content_len;
}

void HttpSession::onRecvContent(const char *data, size_t len) {
    if (_on_recv_body && !_on_recv_body(data, len)) {
        _on_recv_body = nullptr;
    }
}

void HttpSession::onRecv(const Buffer::Ptr &pBuf) {
    _ticker.resetTime();
    input(pBuf->data(), pBuf->size());
}

void HttpSession::onError(const SockException &err) {
    if (_is_live_stream) {
        // flv/ts player
        uint64_t duration = _ticker.createdTime() / 1000;
        WarnP(this) << "FLV/TS/FMP4 Player(" << _media_info.shortUrl() << ") disconnect:" << err << ", time consuming(s):" << duration;

        GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
        if (_total_bytes_usage >= iFlowThreshold * 1024) {
            NOTICE_EMIT(BroadcastFlowReportArgs, Broadcast::kBroadcastFlowReport, _media_info, _total_bytes_usage, duration, true, *this);
        }
        return;
    }
}

void HttpSession::setTimeoutSec(size_t keep_alive_sec) {
    if (!keep_alive_sec) {
        GET_CONFIG(size_t, s_keep_alive_sec, Http::kKeepAliveSecond);
        keep_alive_sec = s_keep_alive_sec;
    }
    _keep_alive_sec = keep_alive_sec;
    getSock()->setSendTimeOutSecond(keep_alive_sec);
}

void HttpSession::setMaxReqSize(size_t max_req_size) {
    if (!max_req_size) {
        GET_CONFIG(size_t, s_max_req_size, Http::kMaxReqSize);
        max_req_size = s_max_req_size;
    }
    _max_req_size = max_req_size;
    setMaxCacheSize(max_req_size);
}

void HttpSession::onManager() {
    if (_ticker.elapsedTime() > _keep_alive_sec * 1000) {
        // http timeout
        shutdown(SockException(Err_timeout, "session timeout"));
    }
}

bool HttpSession::checkWebSocket() {
    auto Sec_WebSocket_Key = _parser["Sec-WebSocket-Key"];
    if (Sec_WebSocket_Key.empty()) {
        return false;
    }
    _is_websocket = true;
    auto Sec_WebSocket_Accept = encodeBase64(SHA1::encode_bin(Sec_WebSocket_Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));

    KeyValue headerOut;
    headerOut["Upgrade"] = "websocket";
    headerOut["Connection"] = "Upgrade";
    headerOut["Sec-WebSocket-Accept"] = Sec_WebSocket_Accept;
    if (!_parser["Sec-WebSocket-Protocol"].empty()) {
        headerOut["Sec-WebSocket-Protocol"] = _parser["Sec-WebSocket-Protocol"];
    }

    auto res_cb = [this]() {
        //Change to reply http header mode first to solve the websocket request pending problem in on-demand playback scenarios: #4553
    };

    auto res_immediately = [this, headerOut]() mutable {
        headerOut.emplace("Cache-Control", "no-store");
        sendResponse(101, false, nullptr, headerOut, nullptr, true);
        _live_over_websocket = true;
    };

    // Determine whether it is websocket-flv
    if (checkLiveStreamFlv(res_cb)) {
        // This is a websocket-flv live request
        res_immediately();
        return true;
    }

    // Determine whether it is websocket-ts
    if (checkLiveStreamTS(res_cb)) {
        // This is a websocket-ts live request
        res_immediately();
        return true;
    }

    // Determine whether it is websocket-fmp4
    if (checkLiveStreamFMP4(res_cb)) {
        // This is a websocket-fmp4 live request
        res_immediately();
        return true;
    }

    // Determine whether it is websocket-fmp4
    if (checkLiveStreamFMP4ByApp(res_cb)) {
        // This is a websocket-fmp4 live request with only app name
        res_immediately();
        return true;
    }

    // This is a normal websocket connection
    if (!onWebSocketConnect(_parser)) {
        sendResponse(501, true, nullptr, headerOut);
        return true;
    }
    sendResponse(101, false, nullptr, headerOut, nullptr, true);
    return true;
}

bool HttpSession::checkLiveStream(const string &schema, const string &url_prefix, const string &url_suffix, const function<void(const MediaSource::Ptr &src)> &cb) {
    std::string url = _parser.url();
    auto it = _parser.getUrlArgs().find("schema");
    if (it != _parser.getUrlArgs().end()) {
        if (strcasecmp(it->second.c_str(), schema.c_str())) {
            // unsupported schema
            return false;
        }
    } else {
        auto prefix_size = url_prefix.size();
        if (prefix_size > 0) {
            if (url.size() < prefix_size || strncasecmp(url.data(), url_prefix.data(), prefix_size)) {
                // Prefix not found
                return false;
            }
            // Remove special prefix from url
            url.erase(0, prefix_size);
        }

        auto suffix_size = url_suffix.size();
        if (suffix_size > 0) {
            if (url.size() < suffix_size || strcasecmp(url.data() + (url.size() - suffix_size), url_suffix.data())) {
                // Suffix not found
                return false;
            }
            // Remove special suffix from url
            url.resize(url.size() - suffix_size);
        }
    }

    GET_CONFIG(string, appName, Protocol::kAppName)
    if (!appName.empty()) {
        auto app_prefix = "/" + appName;
        if (start_with(url, app_prefix)) {
            // Remove special prefix from url
            url.erase(0, app_prefix.size());
        }
    }

    // Url with parameters
    if (!_parser.params().empty()) {
        url += "?";
        url += _parser.params();
    }

    // Url with header Authorization
    auto headers = _parser.getHeader();
    if (!headers["Authorization"].empty() || !headers["authorization"].empty()) {
        auto tmp = !headers["Authorization"].empty() ? headers["Authorization"] : headers["authorization"];
        auto jwt_token = trim(findSubString(tmp.data(), "Bearer", nullptr));
        url += url.find("?") == string::npos ? "?" : "&";
        url += StrPrinter << "token=" << jwt_token;
    }

    if (!headers["User-Agent"].empty()) {
        url += url.find("?") == string::npos ? "?" : "&";
        url += StrPrinter << "user-agent=" << encodeBase64(headers["User-Agent"]);
    }

    // Parse the complete url with protocol + parameters
    _media_info.parse(schema + "://" + _parser["Host"] + url);

    if (_media_info.app.empty() || _media_info.stream.empty()) {
        // URL is invalid
        return false;
    }

    if (_is_websocket) {
        _media_info.protocol = overSsl() ? "wss" : "ws";
    } else {
        _media_info.protocol = overSsl() ? "https" : "http";
    }

    bool close_flag = !strcasecmp(_parser["Connection"].data(), "close");
    weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());

    // Authentication result callback
    auto onRes = [cb, weak_self, close_flag](const string &err) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            // This object has been destroyed
            return;
        }

        if (!err.empty()) {
            if (err == "MaxRequest") {
                // Too many connections
                strong_self->sendResponse(429, close_flag, nullptr, KeyValue(), std::make_shared<HttpStringBody>("429 Too Many Requests"));
                return;
            }
            // Playback authentication failed
            strong_self->sendResponse(401, close_flag, nullptr, KeyValue(), std::make_shared<HttpStringBody>(err));
            return;
        }

        // Asynchronously find live stream
        MediaSource::findAsync(strong_self->_media_info, strong_self, [weak_self, close_flag, cb](const MediaSource::Ptr &src) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // This object has been destroyed
                return;
            }
            if (!src) {
                // Stream not found
                strong_self->sendNotFound(close_flag);
            } else {
                strong_self->_is_live_stream = true;
                strong_self->applyViewOverlayPolicy(src, cb);
            }
        });
    };

    Broadcast::AuthInvoker invoker = [weak_self, onRes](const string &err) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->async([onRes, err]() { onRes(err); }, false);
        }
    };

    auto flag = NOTICE_EMIT(BroadcastMediaPlayedArgs, Broadcast::kBroadcastMediaPlayed, _media_info, invoker, *this);
    if (!flag) {
        // No one is listening to this event, no authentication by default
        invoker("");
    }
    return true;
}

void HttpSession::applyViewOverlayPolicy(const MediaSource::Ptr &source, const std::function<void(const MediaSource::Ptr &)> &cb) {
    if (!source) {
        cb(source);
        return;
    }
    auto params = Parser::parseArgs(_media_info.params);
    const string jwt_token = params.find("token") == params.end() ? "" : params["token"];
    weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());
    Broadcast::ViewOverlayPolicyInvoker policy_cb = [weak_self, source, cb](const Broadcast::ViewOverlayPolicy &policy) {
        auto self = weak_self.lock();
        if (!self) return;
        const bool use_watermark = policy.watermark_enforce && !policy.watermark_excluded;
        const bool use_privacy_mask = policy.privacy_mask_enforce && !policy.privacy_mask_excluded;
        bool use_transcode = false;
#if defined(ENABLE_FFMPEG)
        TranscodeRequest transcode_request;
        std::string transcode_error;
        ProtocolOption transcode_defaults;
        if (!parseTranscodeRequest(self->_media_info.params, transcode_defaults, transcode_request, transcode_error)) {
            self->sendResponse(400, true, nullptr, KeyValue(), make_shared<HttpStringBody>(transcode_error));
            return;
        }
        use_transcode = transcode_request.enabled;
#endif
        if (!use_watermark && !use_privacy_mask && !use_transcode) {
            cb(source);
            return;
        }
#if defined(ENABLE_FFMPEG)
        const CodecId transcode_codec = transcode_request.codec;
        const int transcode_width = transcode_request.width;
        const int transcode_height = transcode_request.height;
        const int transcode_fps = transcode_request.fps;
        const int transcode_bitrate = transcode_request.bitrate;
        const int transcode_gop = transcode_request.gop;
        const string output_schema = self->_media_info.schema;

        vector<OverlayComponent> components;
        OverlayBuildOptions overlay_options;
        overlay_options.resolve_dynamic_tokens = false;
        overlay_options.username = policy.username;
        overlay_options.camera_name = policy.camera_name;
        if (use_watermark && !OverlayPrivacyUtils::parseComponents(policy.watermark_template, components, overlay_options)) {
            self->sendResponse(503, true, nullptr, KeyValue(), make_shared<HttpStringBody>("Watermark template is invalid"));
            return;
        }
        std::string local_image_error;
        if (use_watermark && !OverlayPrivacyUtils::resolveLocalImages(components, local_image_error)) {
            self->sendResponse(503, true, nullptr, KeyValue(), make_shared<HttpStringBody>(local_image_error));
            return;
        }
        if (use_privacy_mask && !OverlayPrivacyUtils::parsePrivacyMasks(policy.privacy_mask_regions,
                                                                          overlay_options.privacy_masks,
                                                                          overlay_options)) {
            self->sendResponse(503, true, nullptr, KeyValue(), make_shared<HttpStringBody>("Privacy mask configuration is invalid"));
            return;
        }
        const string key = string(transcode_codec == CodecH265 ? "h265" : "h264") + "|" +
                           to_string(transcode_width) + "|" + to_string(transcode_height) + "|" +
                           to_string(transcode_bitrate) + "|" + to_string(transcode_fps) + "|" +
                           policy.watermark_template + "|" + policy.privacy_mask_regions;
        const string stream_suffix = ".transcode." + MD5(key).hexdigest();
        DebugL << "Stream " << self->_media_info.stream << " will be transcoded with suffix: " << stream_suffix;
        auto start_transcode = [weak_self, source, cb, overlay_options, stream_suffix, output_schema,
                                transcode_codec, transcode_width, transcode_height,
                                transcode_fps, transcode_bitrate, transcode_gop](const vector<OverlayComponent> &prepared_components) {
            auto self = weak_self.lock();
            if (!self) return;
            TranscodeProcessor::Config cfg;
            cfg.codec = transcode_codec;
            cfg.width = transcode_width;
            cfg.height = transcode_height;
            cfg.fps = transcode_fps;
            cfg.bitrate = transcode_bitrate;
            cfg.gop = transcode_gop;
            cfg.stream_suffix = stream_suffix;
            cfg.output_schema = output_schema;
            cfg.demand = true;
            cfg.overlay_components = prepared_components;
            cfg.overlay_options = overlay_options;
            auto muxer = source->getMuxer();
            if (!muxer) {
                self->sendResponse(503, true, nullptr, KeyValue(), make_shared<HttpStringBody>("View overlay source is unavailable"));
                return;
            }
            auto poller = muxer->getOwnerPoller(MediaSource::NullMediaSource());
            poller->async([weak_self, muxer, cfg, cb]() mutable {
                auto self = weak_self.lock();
                if (!self) return;

                auto on_overlay_ready = [weak_self, cb](const MediaSource::Ptr &derived) {
                    auto self = weak_self.lock();
                    if (!self) return;
                    if (!derived) {
                        self->sendResponse(503, true, nullptr, KeyValue(), make_shared<HttpStringBody>("View overlay source is not ready"));
                        return;
                    }

                    // Keep the response callback on the HTTP session poller.
                    self->async([weak_self, derived, cb]() {
                        auto self = weak_self.lock();
                        if (self) cb(derived);
                    }, false);
                };

                auto derived = muxer->ensureViewOverlayTranscode(cfg);
                if (derived) {
                    on_overlay_ready(derived);
                    return;
                }

                // TranscodeProcessor is created before its first encoded keyframe
                // is available. Wait for the requested protocol source to register.
                const MediaTuple &source_tuple = muxer->getMediaTuple();
                MediaInfo overlay_info;
                overlay_info.schema = cfg.output_schema;
                overlay_info.vhost = source_tuple.vhost;
                overlay_info.app = source_tuple.app;
                overlay_info.stream = source_tuple.stream + cfg.stream_suffix;
                overlay_info.params = source_tuple.params;
                MediaSource::findAsync(overlay_info, self, on_overlay_ready);
            });
        };
        start_transcode(components);
#else
        self->sendResponse(503, true, nullptr, KeyValue(), make_shared<HttpStringBody>("FFmpeg view overlay support is disabled"));
#endif
    };
    auto flag = NOTICE_EMIT(BroadcastMediaViewOverlayArgs, Broadcast::kBroadcastMediaViewOverlay, _media_info, jwt_token, policy_cb, *this);
    if (!flag) {
        policy_cb(Broadcast::ViewOverlayPolicy());
    }
}

// http-fmp4 link format: http://vhost-url:port/media/app/streamid.live.mp4?key1=value1&key2=value2
bool HttpSession::checkLiveStreamFMP4(const function<void()> &cb) {
    auto dur_sec = static_cast<uint64_t>(atoll(_parser.getUrlArgs()["duration"].data()));
    return checkLiveStream(FMP4_SCHEMA, "/media", ".live.mp4", [this, cb, dur_sec](const MediaSource::Ptr &src) {
        auto fmp4_src = dynamic_pointer_cast<FMP4MediaSource>(src);
        assert(fmp4_src);
        if (!cb) {
            // Found the source, send the http header, and send the load later
            sendResponse(200, false, HttpFileManager::getContentType(".mp4").data(), KeyValue(), nullptr, true);
        } else {
            // Custom send http header
            cb();
        }

        // Live streaming sacrifices delay to improve sending performance
        setSocketFlags();
        onWrite(std::make_shared<BufferString>(fmp4_src->getInitSegment()), true);
        weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());
        auto end_dts = std::make_shared<std::atomic<uint64_t>>(std::numeric_limits<uint64_t>::max());
        auto stop_requested = std::make_shared<std::atomic<bool>>(false);

        fmp4_src->pause(false);
        _fmp4_reader = fmp4_src->getRing()->attach(getPoller());
        _fmp4_reader->setGetInfoCB([weak_self]() {
            Any ret;
            ret.set(static_pointer_cast<Session>(weak_self.lock()));
            return ret;
        });
        _fmp4_reader->setDetachCB([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // This object has been destroyed
                return;
            }
            strong_self->shutdown(SockException(Err_shutdown, "fmp4 ring buffer detached"));
        });
        _fmp4_reader->setReadCB([weak_self, fmp4_src, dur_sec, end_dts, stop_requested](const FMP4MediaSource::RingDataType &fmp4_list) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // This object has been destroyed
                return;
            }
            const uint64_t dur_ms = dur_sec * 1000;
            size_t i = 0;
            auto size = fmp4_list->size();
            fmp4_list->for_each([&](const FMP4Packet::Ptr &ts) {
                if (stop_requested->load(std::memory_order_acquire)) {
                    return; // Stop requested: skip the rest of the current batch
                }
                if (dur_ms > 0) {
                    uint64_t expected = std::numeric_limits<uint64_t>::max();
                    uint64_t target_end = ts->time_stamp + dur_ms;

                    if (end_dts->compare_exchange_strong(expected, target_end, std::memory_order_acq_rel)) {
                        DebugL << "http-mp4 set duration limit, end_dts:" << target_end;
                    }
                    const uint64_t limit = end_dts->load(std::memory_order_acquire);
                    if (ts->time_stamp > limit) {
                        if (!stop_requested->exchange(true, std::memory_order_acq_rel)) {
                            WarnL << "http-mp4 duration limit reached, time_stamp:" << ts->time_stamp << ", limit:" << limit;
                            fmp4_src->getOwnerPoller()->async([fmp4_src]() { fmp4_src->close(false); });
                            strong_self->shutdown(SockException(Err_shutdown, "fmp4 duration limit reached"));
                        }
                        return;
                    }
                }
                strong_self->onWrite(ts, ++i == size);
            });
        });
    });
}

// http-ts link format: http://vhost-url:port/media/app/streamid.live.ts?key1=value1&key2=value2
bool HttpSession::checkLiveStreamTS(const function<void()> &cb) {
    return checkLiveStream(TS_SCHEMA, "/media", ".live.ts", [this, cb](const MediaSource::Ptr &src) {
        auto ts_src = dynamic_pointer_cast<TSMediaSource>(src);
        assert(ts_src);
        if (!cb) {
            // Found the source, send the http header, and send the load later
            sendResponse(200, false, HttpFileManager::getContentType(".ts").data(), KeyValue(), nullptr, true);
        } else {
            // Custom send http header
            cb();
        }

        // Live streaming sacrifices delay to improve sending performance
        setSocketFlags();
        weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());
        ts_src->pause(false);
        _ts_reader = ts_src->getRing()->attach(getPoller());
        _ts_reader->setGetInfoCB([weak_self]() {
            Any ret;
            ret.set(static_pointer_cast<Session>(weak_self.lock()));
            return ret;
        });
        _ts_reader->setDetachCB([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // This object has been destroyed
                return;
            }
            strong_self->shutdown(SockException(Err_shutdown, "ts ring buffer detached"));
        });
        _ts_reader->setReadCB([weak_self](const TSMediaSource::RingDataType &ts_list) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // This object has been destroyed
                return;
            }
            size_t i = 0;
            auto size = ts_list->size();
            ts_list->for_each([&](const TSPacket::Ptr &ts) { strong_self->onWrite(ts, ++i == size); });
        });
    });
}

// http-flv link format: http://vhost-url:port/media/app/streamid.live.flv?key1=value1&key2=value2
bool HttpSession::checkLiveStreamFlv(const function<void()> &cb) {
    auto start_pts = atoll(_parser.getUrlArgs()["startPts"].data());
    return checkLiveStream(RTMP_SCHEMA, "/media", ".live.flv", [this, cb, start_pts](const MediaSource::Ptr &src) {
        auto rtmp_src = dynamic_pointer_cast<RtmpMediaSource>(src);
        assert(rtmp_src);
        if (!cb) {
            // Found the source, send the http header, and send the load later
            KeyValue headerOut;
            headerOut["Cache-Control"] = "no-store";
            sendResponse(200, false, HttpFileManager::getContentType(".flv").data(), headerOut, nullptr, true);
        } else {
            // Custom send http header
            cb();
        }
        // Live streaming sacrifices delay to improve sending performance
        setSocketFlags();

        // Print warning log when it is not H264/AAC, to prevent users from raising invalid issues
        auto tracks = src->getTracks(false);
        for (auto &track : tracks) {
            switch (track->getCodecId()) {
                case CodecH264:
                case CodecAAC: break;
                default: {
                    WarnP(this) << "FLV players generally only support H264 and AAC encoding, and this encoding format may not be supported by the player.:" << track->getCodecName();
                    break;
                }
            }
        }

        start(getPoller(), rtmp_src, start_pts);
    });
}

bool HttpSession::checkLiveStreamHls() {
    std::string url = _parser.url();
    string url_prefix = "/media";
    string hls_suffix = "/hls.m3u8";
    string ts_suffix = ".ts";
    string hlsfmp4_suffix = "/hls.fmp4.m3u8";
    string fmp4_suffix = ".mp4";
    auto it = _parser.getUrlArgs().find("schema");
    if (it != _parser.getUrlArgs().end()) {
        if (strcasecmp(it->second.c_str(), HLS_SCHEMA)) {
            // unsupported schema
            return false;
        }
        if (strcasecmp(it->second.c_str(), HLS_FMP4_SCHEMA)) {
            // unsupported schema
            return false;
        }
    } else {
        auto prefix_size = url_prefix.size();
        if (prefix_size > 0) {
            if (url.size() < prefix_size || strncasecmp(url.data(), url_prefix.data(), prefix_size)) {
                // Prefix not found
                return false;
            }
            // Remove special prefix from url
            url.erase(0, prefix_size);
        }
        if (!end_with(url, hls_suffix) && !end_with(url, ts_suffix) && !end_with(url, hlsfmp4_suffix) && !end_with(url, fmp4_suffix)) {
            // Suffix not found
            return false;
        }
    }

    GET_CONFIG(string, appName, Protocol::kAppName)
    if (!appName.empty()) {
        auto app_prefix = "/" + appName;
        if (start_with(url, app_prefix)) {
            // Remove special prefix from url
            url.erase(0, app_prefix.size());
        }
    }

    string schema;
    if (end_with(url, hls_suffix) || end_with(url, ts_suffix)) {
        schema = HLS_SCHEMA;
    } else {
        schema = HLS_FMP4_SCHEMA;
    }

    // Url with parameters
    if (!_parser.params().empty()) {
        url += "?";
        url += _parser.params();
    }

    // Url with header Authorization
    auto headers = _parser.getHeader();
    if (!headers["Authorization"].empty() || !headers["authorization"].empty()) {
        auto tmp = !headers["Authorization"].empty() ? headers["Authorization"] : headers["authorization"];
        auto jwt_token = trim(findSubString(tmp.data(), "Bearer", nullptr));
        url += url.find("?") == string::npos ? "?" : "&";
        url += StrPrinter << "token=" << jwt_token;
    }

    if (!headers["User-Agent"].empty()) {
        url += url.find("?") == string::npos ? "?" : "&";
        url += StrPrinter << "user-agent=" << encodeBase64(headers["User-Agent"]);
    }

    // Parse the complete url with protocol + parameters
    _media_info.parse(schema + "://" + _parser["Host"] + url);

    if (_media_info.app.empty() || _media_info.stream.empty()) {
        // URL is invalid
        return false;
    }

    _media_info.protocol = overSsl() ? "https" : "http";

    return true;
}

void HttpSession::onHttpRequest_GET() {
    // First check if it is a WebSocket request
    if (checkWebSocket()) {
        // The following are all websocket body data
        _on_recv_body = [this](const char *data, size_t len) {
            WebSocketSplitter::decode((uint8_t *)data, len);
            // _contentCallBack is sustainable, and subsequent data needs to be processed later
            return true;
        };
        return;
    }

    if (emitHttpEvent(false)) {
        // Intercept http api events
        return;
    }

    if (checkLiveMotionStream()) {
        // Intercept MJPEG motion stream
        return;
    }

    if (checkLiveStreamFlv()) {
        // Intercept http-flv player
        return;
    }

    if (checkLiveStreamTS()) {
        // Intercept http-ts player
        return;
    }

    if (checkLiveStreamFMP4()) {
        // Intercept http-fmp4 player
        return;
    }

    if (checkLiveStreamFMP4ByApp()) {
        // Intercept http-fmp4 player by app
        return;
    }

    if (checkLiveMotionStreamByApp()) {
        // Intercept MJPEG motion stream by app
        return;
    }

    if (checkLiveStreamHlsByApp()) {
        // Intercept hls-ts, hls-fmp4 player by app
        return;
    }

    if (checkLiveStreamHls()) {
        // Intercept hls-ts, hls-fmp4 player
    }

    bool bClose = !strcasecmp(_parser["Connection"].data(), "close");
    weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());
    HttpFileManager::onAccessPath(*this, _parser, _media_info, [weak_self, bClose](int code, const string &content_type,
                                                                      const StrCaseMap &responseHeader, const HttpBody::Ptr &body) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->async([weak_self, bClose, code, content_type, responseHeader, body]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->sendResponse(code, bClose, content_type.data(), responseHeader, body);
        });
    });
}

static string dateStr() {
    char buf[64];
    time_t tt = time(NULL);
    strftime(buf, sizeof buf, "%a, %b %d %Y %H:%M:%S GMT", gmtime(&tt));
    return buf;
}

class AsyncSenderData {
public:
    friend class AsyncSender;
    using Ptr = std::shared_ptr<AsyncSenderData>;
    AsyncSenderData(HttpSession::Ptr session, const HttpBody::Ptr &body, bool close_when_complete) {
        _session = std::move(session);
        _body = body;
        _close_when_complete = close_when_complete;
    }

private:
    std::weak_ptr<HttpSession> _session;
    HttpBody::Ptr _body;
    bool _close_when_complete;
    bool _read_complete = false;
};

class AsyncSender {
public:
    using Ptr = std::shared_ptr<AsyncSender>;
    static bool onSocketFlushed(const AsyncSenderData::Ptr &data) {
        if (data->_read_complete) {
            if (data->_close_when_complete) {
                // Close socket after sending is complete
                shutdown(data->_session.lock());
            }
            return false;
        }

        GET_CONFIG(uint32_t, sendBufSize, Http::kSendBufSize);
        data->_body->readDataAsync(sendBufSize, [data](const Buffer::Ptr &sendBuf) {
            auto session = data->_session.lock();
            if (!session) {
                // This object has been destroyed
                return;
            }
            session->async([data, sendBuf]() {
                auto session = data->_session.lock();
                if (!session) {
                    // This object has been destroyed
                    return;
                }
                onRequestData(data, session, sendBuf);
            }, false);
        });
        return true;
    }

private:
    static void onRequestData(const AsyncSenderData::Ptr &data, const std::shared_ptr<HttpSession> &session, const Buffer::Ptr &sendBuf) {
        session->_ticker.resetTime();
        if (sendBuf && session->send(sendBuf) != -1) {
            // The file has not been read completely, and needs to be sent continuously
            if (!session->isSocketBusy()) {
                // Socket can still write, continue to request data
                onSocketFlushed(data);
            }
            return;
        }
        // The file is written
        data->_read_complete = true;
        if (!session->isSocketBusy() && data->_close_when_complete) {
            shutdown(session);
        }
    }

    static void shutdown(const std::shared_ptr<HttpSession> &session) {
        if (session) {
            session->shutdown(SockException(Err_shutdown, StrPrinter << "close connection after send http body completed."));
        }
    }
};

void HttpSession::sendResponse(int code,
                               bool bClose,
                               const char *pcContentType,
                               const HttpSession::KeyValue &header,
                               const HttpBody::Ptr &body,
                               bool no_content_length) {
    if (_live_over_websocket) {
        WebSocketHeader ws_header;
        ws_header._fin = true;
        ws_header._reserved = 0;
        ws_header._opcode = WebSocketHeader::CLOSE;
        ws_header._mask_flag = false;
        uint16_t why = htons(0xFFFF & code);
        std::string buffer;
        buffer.append(reinterpret_cast<char *>(&why), 2);
        if (body && code != 404) {
            buffer.append(body->readData(body->remainSize())->toString());
        } else {
            buffer.append("unknown reason");
        }
        WebSocketSplitter::encode(ws_header, std::make_shared<BufferString>(std::move(buffer)));
        return;
    }
    GET_CONFIG(string, charSet, Http::kCharSet);
    GET_CONFIG(uint32_t, keepAliveSec, Http::kKeepAliveSecond);

    // Body defaults to empty
    int64_t size = 0;
    if (body && body->remainSize()) {
        // There is a body, get the body size
        size = body->remainSize();
    }

    if (no_content_length) {
        // Http-flv live broadcast is Keep-Alive type
        bClose = false;
    } else if ((size_t)size >= SIZE_MAX || size < 0) {
        // If the body is not fixed length, then the socket should be closed after sending the body, so that the browser can judge the download completion
        bClose = true;
    }

    HttpSession::KeyValue &headerOut = const_cast<HttpSession::KeyValue &>(header);
    headerOut.emplace("Date", dateStr());
    headerOut.emplace("Server", kServerName);
    headerOut.emplace("Connection", bClose ? "close" : "keep-alive");

    GET_CONFIG(bool, allow_cross_domains, Http::kAllowCrossDomains);
    if (allow_cross_domains && !_origin.empty()) {
        headerOut.emplace("Access-Control-Allow-Origin", _origin);
        headerOut.emplace("Access-Control-Allow-Credentials", "true");
    }

    if (!bClose) {
        string keepAliveString = "timeout=";
        keepAliveString += to_string(keepAliveSec);
        keepAliveString += ", max=100";
        headerOut.emplace("Keep-Alive", std::move(keepAliveString));
    }

    if (!no_content_length && size >= 0 && (size_t)size < SIZE_MAX) {
        // The file length is a fixed value, and it is not http-flv that forcibly sets Content-Length
        headerOut["Content-Length"] = to_string(size);
    }

    if (size && !pcContentType) {
        // When there is a body, set the default type
        pcContentType = "text/plain";
    }

    if ((size || no_content_length) && pcContentType) {
        // When there is a body, set the file type
        string strContentType = pcContentType;
        strContentType += "; charset=";
        strContentType += charSet;
        headerOut.emplace("Content-Type", std::move(strContentType));
    }

    // Send http header
    string str;
    str.reserve(256);
    str += "HTTP/1.1 ";
    str += to_string(code);
    str += ' ';
    str += HttpConst::getHttpStatusMessage(code);
    str += "\r\n";
    for (auto &pr : header) {
        str += pr.first;
        str += ": ";
        str += pr.second;
        str += "\r\n";
    }
    str += "\r\n";
    SockSender::send(std::move(str));
    _ticker.resetTime();

    if (!size) {
        // No body
        if (bClose) {
            shutdown(SockException(Err_shutdown, StrPrinter << "close connection after send http header completed with status code:" << code));
        }
        return;
    }

#if 0
    // Sendfile has no performance advantage over shared mmap, on the contrary, sendfile also has functional defects, so it is blocked first
    if (typeid(*this) == typeid(HttpSession) && !body->sendFile(getSock()->rawFD())) {
        // Http supports sendfile optimization
        return;
    }
#endif

    GET_CONFIG(uint32_t, sendBufSize, Http::kSendBufSize);
    if (body->remainSize() > sendBufSize) {
        // File download improves sending performance
        setSocketFlags();
    }

    // Send http body
    AsyncSenderData::Ptr data = std::make_shared<AsyncSenderData>(static_pointer_cast<HttpSession>(shared_from_this()), body, bClose);
    getSock()->setOnFlush([data]() { return AsyncSender::onSocketFlushed(data); });
    AsyncSender::onSocketFlushed(data);
}

void HttpSession::urlDecode(Parser &parser) {
    parser.setUrl(strCoding::UrlDecodePath(parser.url()));
    for (auto &pr : _parser.getUrlArgs()) {
        const_cast<string &>(pr.second) = strCoding::UrlDecodeComponent(pr.second);
    }
}

bool HttpSession::emitHttpEvent(bool doInvoke) {
    bool bClose = !strcasecmp(_parser["Connection"].data(), "close");
    // ///////////////////Asynchronous reply Invoker///////////////////////////////
    weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());
    HttpResponseInvoker invoker = [weak_self, bClose](int code, const KeyValue &headerOut, const HttpBody::Ptr &body) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->async([weak_self, bClose, code, headerOut, body]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // This object has been destroyed
                return;
            }
            strong_self->sendResponse(code, bClose, nullptr, headerOut, body);
        });
    };
    // /////////////////Broadcast HTTP event///////////////////////////
    bool consumed = false; // Is this event consumed?
    NOTICE_EMIT(BroadcastHttpRequestArgs, Broadcast::kBroadcastHttpRequest, _parser, invoker, consumed, *this);
    if (!consumed && doInvoke) {
        // This event is not consumed, so return 404
        invoker(404, KeyValue(), HttpBody::Ptr());
    }
    return consumed;
}

std::string HttpSession::get_peer_ip() {
    GET_CONFIG(string, forwarded_ip_header, Http::kForwardedIpHeader);
    if (!forwarded_ip_header.empty() && !_parser.getHeader()[forwarded_ip_header].empty()) {
        return _parser.getHeader()[forwarded_ip_header];
    }
    return Session::get_peer_ip();
}

void HttpSession::onHttpRequest_POST() {
    emitHttpEvent(true);
}

void HttpSession::sendNotFound(bool bClose) {
    GET_CONFIG(string, notFound, Http::kNotFound);
    sendResponse(404, bClose, "text/html", KeyValue(), std::make_shared<HttpStringBody>(notFound));
}

void HttpSession::setSocketFlags() {
    GET_CONFIG(int, mergeWriteMS, General::kMergeWriteMS);
    if (mergeWriteMS > 0) {
        // In push mode, closing TCP_NODELAY will increase the delay of the push end, but the server performance will be improved
        SockUtil::setNoDelay(getSock()->rawFD(), false);
        // In playback mode, enabling MSG_MORE will increase the delay, but it can improve sending performance
        setSendFlags(SOCKET_DEFAULE_FLAGS | FLAG_MORE);
    }
}

void HttpSession::onWrite(const Buffer::Ptr &buffer, bool flush) {
    if (flush) {
        // Need to flush, then flush the cache once
        HttpSession::setSendFlushFlag(true);
    }

    _ticker.resetTime();
    if (!_live_over_websocket) {
        _total_bytes_usage += buffer->size();
        send(buffer);
    } else {
        WebSocketHeader header;
        header._fin = true;
        header._reserved = 0;
        header._opcode = WebSocketHeader::BINARY;
        header._mask_flag = false;
        WebSocketSplitter::encode(header, buffer);
    }

    if (flush) {
        // After this cache flush, the next time you don't need to flush the cache
        HttpSession::setSendFlushFlag(false);
    }
}

void HttpSession::onWebSocketEncodeData(Buffer::Ptr buffer) {
    _total_bytes_usage += buffer->size();
    send(std::move(buffer));
}

void HttpSession::onWebSocketDecodeComplete(const WebSocketHeader &header_in) {
    WebSocketHeader &header = const_cast<WebSocketHeader &>(header_in);
    header._mask_flag = false;

    switch (header._opcode) {
        case WebSocketHeader::CLOSE: {
            encode(header, nullptr);
            shutdown(SockException(Err_shutdown, "recv close request from client"));
            break;
        }

        default: break;
    }
}

// ---------------------------------------------------------------------------
// Motion MJPEG stream
// URL: /media/{app}/{stream}.motion.mjpeg?overlay_motion=0|1&overlay_roi=0|1
// Streams a multipart/x-mixed-replace JPEG sequence with per-frame headers:
//   X-Motion: 0|1   X-Timestamp: <ms>   X-Active-Cells: <N>
// ---------------------------------------------------------------------------

bool HttpSession::checkLiveMotionStream() {
#ifdef ENABLE_MOTION
    bool overlay_motion = !!atoi(_parser.getUrlArgs()["overlay_motion"].data());
    bool overlay_roi    = !!atoi(_parser.getUrlArgs()["overlay_roi"].data());

    return checkLiveStream(MOTION_MJPEG_SCHEMA, "/media", ".motion.mjpeg",
        [this, overlay_motion, overlay_roi](const MediaSource::Ptr &src) {
            auto motion_src = dynamic_pointer_cast<MotionMjpegMediaSource>(src);
            if (!motion_src || !motion_src->getRing()) {
                sendNotFound(true);
                return;
            }

            motion_src->setOverlay(overlay_motion, overlay_roi);

            KeyValue header;
            header["Cache-Control"]               = "no-store";
            header["Access-Control-Allow-Origin"] = "*";
            sendResponse(200, false,
                "multipart/x-mixed-replace; boundary=mjpeg_boundary",
                header, nullptr, /*no_content_length=*/true);

            setSocketFlags();

            weak_ptr<HttpSession> weak_self =
                static_pointer_cast<HttpSession>(shared_from_this());

            _motion_reader = motion_src->getRing()->attach(getPoller());
            _motion_reader->setGetInfoCB([weak_self]() {
                Any ret;
                ret.set(static_pointer_cast<Session>(weak_self.lock()));
                return ret;
            });
            _motion_reader->setDetachCB([weak_self]() {
                auto strong_self = weak_self.lock();
                if (!strong_self) return;
                strong_self->shutdown(SockException(Err_shutdown, "motion ring buffer detached"));
            });
            _motion_reader->setReadCB([weak_self](const MotionJpegFrame::Ptr &pkt) {
                auto strong_self = weak_self.lock();
                if (!strong_self || !pkt || !pkt->jpeg) return;

                const auto &jpeg = pkt->jpeg;
                string hdr =
                    "--mjpeg_boundary\r\n"
                    "Content-Type: image/jpeg\r\n"
                    "Content-Length: " + to_string(jpeg->size()) + "\r\n"
                    "X-Motion: "       + string(pkt->motion ? "1" : "0") + "\r\n"
                    "X-Timestamp: "    + to_string(pkt->stamp_ms) + "\r\n"
                    "X-Active-Cells: " + to_string(pkt->active_cells) + "\r\n"
                    "\r\n";
                strong_self->onWrite(std::make_shared<BufferString>(std::move(hdr)), false);
                strong_self->onWrite(std::make_shared<MjpegBuffer>(jpeg), false);
                strong_self->onWrite(std::make_shared<BufferString>("\r\n"), true);
            });
        });
#else
    return false;
#endif // ENABLE_MOTION
}

// ---------------------------------------------------------------------------
// API v2
// ---------------------------------------------------------------------------
bool HttpSession::checkLiveStreamByApp(const string &schema, const string &url_prefix, const string &url_suffix, const function<void(const vector<MediaSource::Ptr> &)> &cb) {
    std::string url = _parser.url();
    auto it = _parser.getUrlArgs().find("schema");
    if (it != _parser.getUrlArgs().end()) {
        if (strcasecmp(it->second.c_str(), schema.c_str())) {
            // unsupported schema
            return false;
        }
    } else {
        auto prefix_size = url_prefix.size();
        if (prefix_size > 0) {
            if (url.size() < prefix_size || strncasecmp(url.data(), url_prefix.data(), prefix_size)) {
                // Prefix not found
                return false;
            }
            // Remove special prefix from url
            url.erase(0, prefix_size);
        }

        auto suffix_size = url_suffix.size();
        if (suffix_size > 0) {
            if (url.size() < suffix_size || strcasecmp(url.data() + (url.size() - suffix_size), url_suffix.data())) {
                // Suffix not found
                return false;
            }
            // Remove special suffix from url
            url.erase(url.size() - suffix_size);
        }
    }

    GET_CONFIG(string, appName, Protocol::kAppName)
    if (!appName.empty()) {
        auto app_prefix = "/" + appName;
        if (start_with(url, app_prefix)) {
            // Remove special prefix from url
            url.erase(0, app_prefix.size());
        }
    }
    
    // Url with parameters
    if (!_parser.params().empty()) {
        url += "?";
        url += _parser.params();
    }

    // Url with header Authorization
    auto headers = _parser.getHeader();
    if (!headers["Authorization"].empty() || !headers["authorization"].empty()) {
        auto tmp = !headers["Authorization"].empty() ? headers["Authorization"] : headers["authorization"];
        auto jwt_token = trim(findSubString(tmp.data(), "Bearer", nullptr));
        url += url.find("?") == string::npos ? "?" : "&";
        url += StrPrinter << "token=" << jwt_token;
    }

    if (!headers["User-Agent"].empty()) {
        url += url.find("?") == string::npos ? "?" : "&";
        url += StrPrinter << "user-agent=" << encodeBase64(headers["User-Agent"]);
    }

    // Parse the complete url with protocol + parameters
    _media_info.parse(schema + "://" + _parser["Host"] + url);

    // note: app are required for live stream, but stream name can be empty (e.g. for motion stream)
    GET_CONFIG(string, record_app, Record::kAppName);
    auto is_vod = _media_info.app == record_app;
    if (_media_info.app.empty() || (is_vod && _media_info.stream.empty())) {
        // URL is invalid
        return false;
    }

    if (_is_websocket) {
        _media_info.protocol = overSsl() ? "wss" : "ws";
    } else {
        _media_info.protocol = overSsl() ? "https" : "http";
    }

    bool close_flag = !strcasecmp(_parser["Connection"].data(), "close");
    weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());

    // Authentication result callback
    auto onRes = [cb, weak_self, close_flag](const string &err) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        if (!err.empty()) {
           if (err == "MaxRequest") {
                // Too many connections
                strong_self->sendResponse(429, close_flag, nullptr, KeyValue(), std::make_shared<HttpStringBody>("429 Too Many Requests"));
                return;
            }
            // Playback authentication failed
            strong_self->sendResponse(401, close_flag, nullptr, KeyValue(), std::make_shared<HttpStringBody>(err));
            return;
        }

        // Asynchronously find live stream
        MediaSource::findAsyncByApp(strong_self->_media_info, strong_self, [weak_self, close_flag, cb](const std::vector<MediaSource::Ptr> &list_src) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // This object has been destroyed
                return;
            }
            if (list_src.empty()) {
                // Stream not found
                strong_self->sendNotFound(close_flag);
            } else {
                strong_self->_is_live_stream = true;
                // Trigger callback
                cb(list_src);
            }
        });
    };

    Broadcast::AuthInvoker invoker = [weak_self, onRes](const string &err) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->async([onRes, err]() { onRes(err); }, false);
        }
    };

    auto flag = NOTICE_EMIT(BroadcastMediaPlayedArgs, Broadcast::kBroadcastMediaPlayed, _media_info, invoker, *this);
    if (!flag) {
        // No one is listening to this event, no authentication by default
        invoker("");
    }
    return true;
}

// FMP4 live stream (app-level, no stream name required)
// URL format: http://vhost-url:port/media/app.live.mp4?duration=xx&quality=hi|lo|auto&prefered=hi|lo
bool HttpSession::checkLiveStreamFMP4ByApp(const std::function<void()> &fmp4_list) {
    auto dur_sec  = static_cast<uint64_t>(atoll(_parser.getUrlArgs()["duration"].data()));
    auto quality  = _parser.getUrlArgs().find("quality") != _parser.getUrlArgs().end() ? _parser.getUrlArgs()["quality"]  : "auto";
    auto prefered = _parser.getUrlArgs().find("prefered") != _parser.getUrlArgs().end() ? _parser.getUrlArgs()["prefered"] : "hi";
    return checkLiveStreamByApp(FMP4_SCHEMA, "/media", ".live2.mp4", [this, fmp4_list, dur_sec, quality, prefered](const vector<MediaSource::Ptr> &list_src) {
        // Find a source whose MediaTuple.params contains quality=<target>
        auto findByQuality = [&](const string &target) -> MediaSource::Ptr {
            for (auto &src : list_src) {
                auto kv = Parser::parseArgs(src->getMediaTuple().params);
                auto it = kv.find("quality");
                if (it != kv.end() && it->second == target) {
                    return src;
                }
            }
            return nullptr;
        };

        MediaSource::Ptr selected;
        if (quality == "hi" || quality == "lo") {
            selected = findByQuality(quality);
        } else {
            // auto: try prefered first, then fall back to the other
            selected = findByQuality(prefered);
            if (!selected) {
                selected = findByQuality(prefered == "hi" ? "lo" : "hi");
            }
            // use "auto" for replay stream without quality specified in params
            if (!selected) {
                selected = findByQuality("auto");
            }
        }

        if (!selected) {
            sendNotFound(true);
            return;
        }

        auto serve_source = [this, fmp4_list, dur_sec](const MediaSource::Ptr &source) {
            auto fmp4_src = dynamic_pointer_cast<FMP4MediaSource>(source);
            if (!fmp4_src) {
                sendNotFound(true);
                return;
            }

            if (!fmp4_list) {
                sendResponse(200, false, HttpFileManager::getContentType(".mp4").data(), KeyValue(), nullptr, true);
            } else {
                fmp4_list();
            }

            setSocketFlags();
            onWrite(std::make_shared<BufferString>(fmp4_src->getInitSegment()), true);

            weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());
            auto end_dts        = std::make_shared<std::atomic<uint64_t>>(std::numeric_limits<uint64_t>::max());
            auto stop_requested = std::make_shared<std::atomic<bool>>(false);

            fmp4_src->pause(false);
            _fmp4_reader = fmp4_src->getRing()->attach(getPoller());
            _fmp4_reader->setGetInfoCB([weak_self]() {
                Any ret;
                ret.set(static_pointer_cast<Session>(weak_self.lock()));
                return ret;
            });
            _fmp4_reader->setDetachCB([weak_self]() {
                auto strong_self = weak_self.lock();
                if (!strong_self) return;
                strong_self->shutdown(SockException(Err_shutdown, "fmp4 ring buffer detached"));
            });
            _fmp4_reader->setMessageCB([weak_self](const Any &data) {
                auto strong_self = weak_self.lock();
                if (!strong_self) return;
                if (data.is<std::string>()) {
                    auto &init_seg = data.get<std::string>();
                    if (!init_seg.empty()) {
                        DebugL << "Received new init segment, length: " << init_seg.size();
                        strong_self->onWrite(std::make_shared<BufferString>(init_seg), true);
                    }
                }
            });
            _fmp4_reader->setReadCB([weak_self, fmp4_src, dur_sec, end_dts, stop_requested]
                                    (const FMP4MediaSource::RingDataType &fmp4_list) {
                auto strong_self = weak_self.lock();
                if (!strong_self) return;
                const uint64_t dur_ms = dur_sec * 1000;
                size_t i = 0;
                auto size = fmp4_list->size();
                fmp4_list->for_each([&](const FMP4Packet::Ptr &ts) {
                    if (stop_requested->load(std::memory_order_acquire)) return;
                    if (dur_ms > 0) {
                        uint64_t expected = std::numeric_limits<uint64_t>::max();
                        uint64_t target_end = ts->time_stamp + dur_ms;
                        if (end_dts->compare_exchange_strong(expected, target_end, std::memory_order_acq_rel)) {
                            DebugL << "http-mp4 set duration limit, end_dts:" << target_end;
                        }
                        const uint64_t limit = end_dts->load(std::memory_order_acquire);
                        if (ts->time_stamp > limit) {
                            if (!stop_requested->exchange(true, std::memory_order_acq_rel)) {
                                WarnL << "http-mp4 duration limit reached, time_stamp:" << ts->time_stamp << ", limit:" << limit;
                                fmp4_src->getOwnerPoller()->async([fmp4_src]() { fmp4_src->close(false); });
                                strong_self->shutdown(SockException(Err_shutdown, "fmp4 duration limit reached"));
                            }
                            return;
                        }
                    }
                    strong_self->onWrite(ts, ++i == size);
                });
            });
        };

        applyViewOverlayPolicy(selected, serve_source);
    });
}

// HLS master playlist (app-level, no stream name required)
// URL: /media/{app}/hls.master.m3u8
// Returns an HLS master playlist listing all sub-streams for the app.
// Sub-stream entries use absolute paths: /media/{app}/{stream}/hls.m3u8
bool HttpSession::checkLiveStreamHlsByApp() {
    // Capture base URL before checkLiveStreamByApp may alter _media_info
    string base_url = _parser.url();
    static const string kMasterSuffix = "/hls.master.m3u8";
    if (end_with(base_url, kMasterSuffix)) {
        base_url.resize(base_url.size() - kMasterSuffix.size());
    }

    bool close_flag = !strcasecmp(_parser["Connection"].data(), "close");
    return checkLiveStreamByApp(HLS_SCHEMA, "/media", "/hls.master.m3u8",
        [this, close_flag, base_url](const vector<MediaSource::Ptr> &list_src) {
            string playlist =
                "#EXTM3U\r\n"
                "#EXT-X-VERSION:3\r\n";

            for (const auto &src : list_src) {
                const auto &tuple = src->getMediaTuple();
                if (tuple.stream.empty()) continue;

                // Parse quality params once
                auto kv = Parser::parseArgs(tuple.params);
                auto quality_it = kv.find("quality");

                // Accumulate bandwidth from all tracks; pick resolution from video track only
                int bandwidth = 0;
                string resolution;
                auto tracks = src->getTracks();
                for (const auto &track : tracks) {
                    int br = track->getBitRate();
                    if (br > 0) {
                        bandwidth += br;
                    }
                    if (track->getTrackType() == TrackVideo) {
                        auto video_track = dynamic_pointer_cast<VideoTrack>(track);
                        int w = video_track->getVideoWidth();
                        int h =  video_track->getVideoHeight();
                        if (w > 0 && h > 0) {
                            resolution = to_string(w) + "x" + to_string(h);
                        }
                    }
                }

                // Fallback bandwidth based on quality param
                if (bandwidth <= 0) {
                    bandwidth = (quality_it != kv.end() && quality_it->second == "lo") ? 512000 : 2000000;
                }

                string attrs = "BANDWIDTH=" + to_string(bandwidth);
                if (!resolution.empty()) {
                    attrs += ",RESOLUTION=" + resolution;
                }
                // Add human-readable NAME from quality param if available
                if (quality_it != kv.end() && !quality_it->second.empty()) {
                    attrs += ",NAME=\"" + quality_it->second + "\"";
                }

                playlist += "#EXT-X-STREAM-INF:" + attrs + "\r\n";
                playlist += base_url + "/" + tuple.stream + "/hls.m3u8\r\n";
            }

            KeyValue header;
            header["Cache-Control"] = "no-store";
            sendResponse(200, close_flag, "application/vnd.apple.mpegurl", header, std::make_shared<HttpStringBody>(playlist));
        });
}

// ---------------------------------------------------------------------------
// Motion MJPEG stream (app-level, no stream name required)
// URL: /media/{app}.motion.mjpeg?overlay_motion=0|1&overlay_roi=0|1
// ---------------------------------------------------------------------------
bool HttpSession::checkLiveMotionStreamByApp() {
#ifdef ENABLE_MOTION
    bool overlay_motion = !!atoi(_parser.getUrlArgs()["overlay_motion"].data());
    bool overlay_roi    = !!atoi(_parser.getUrlArgs()["overlay_roi"].data());

    return checkLiveStreamByApp(MOTION_MJPEG_SCHEMA, "/media", ".motion.mjpeg",
        [this, overlay_motion, overlay_roi](const vector<MediaSource::Ptr> &list_src) {
            // Use the first MotionMjpegMediaSource found in the app
            MotionMjpegMediaSource::Ptr motion_src;
            for (auto &src : list_src) {
                motion_src = dynamic_pointer_cast<MotionMjpegMediaSource>(src);
                if (motion_src && motion_src->getRing()) break;
                motion_src = nullptr;
            }
            if (!motion_src) {
                sendNotFound(true);
                return;
            }

            motion_src->setOverlay(overlay_motion, overlay_roi);

            KeyValue header;
            header["Cache-Control"]               = "no-store";
            header["Access-Control-Allow-Origin"] = "*";
            sendResponse(200, false,
                "multipart/x-mixed-replace; boundary=mjpeg_boundary",
                header, nullptr, /*no_content_length=*/true);

            setSocketFlags();

            weak_ptr<HttpSession> weak_self = static_pointer_cast<HttpSession>(shared_from_this());

            _motion_reader = motion_src->getRing()->attach(getPoller());
            _motion_reader->setGetInfoCB([weak_self]() {
                Any ret;
                ret.set(static_pointer_cast<Session>(weak_self.lock()));
                return ret;
            });
            _motion_reader->setDetachCB([weak_self]() {
                auto strong_self = weak_self.lock();
                if (!strong_self) return;
                strong_self->shutdown(SockException(Err_shutdown, "motion ring buffer detached"));
            });
            _motion_reader->setReadCB([weak_self](const MotionJpegFrame::Ptr &pkt) {
                auto strong_self = weak_self.lock();
                if (!strong_self || !pkt || !pkt->jpeg) return;

                const auto &jpeg = pkt->jpeg;
                string hdr =
                    "--mjpeg_boundary\r\n"
                    "Content-Type: image/jpeg\r\n"
                    "Content-Length: " + to_string(jpeg->size()) + "\r\n"
                    "X-Motion: "       + string(pkt->motion ? "1" : "0") + "\r\n"
                    "X-Timestamp: "    + to_string(pkt->stamp_ms) + "\r\n"
                    "X-Active-Cells: " + to_string(pkt->active_cells) + "\r\n"
                    "\r\n";
                strong_self->onWrite(std::make_shared<BufferString>(std::move(hdr)), false);
                strong_self->onWrite(std::make_shared<MjpegBuffer>(jpeg), false);
                strong_self->onWrite(std::make_shared<BufferString>("\r\n"), true);
            });
        });
#else
    return false;
#endif // ENABLE_MOTION
}

// ---------------------------------------------------------------------------

void HttpSession::onDetach() {
    shutdown(SockException(Err_shutdown, "rtmp ring buffer detached"));
}

std::shared_ptr<FlvMuxer> HttpSession::getSharedPtr() {
    return dynamic_pointer_cast<FlvMuxer>(shared_from_this());
}

} /* namespace mediakit */
