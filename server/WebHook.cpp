#include <sstream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Http/HttpSession.h"
#include "Http/HttpRequester.h"
#include "Network/Session.h"
#include "Rtsp/RtspSession.h"
#include "Server/GlobalMonitor.h"
#include "WebHook.h"
#include "WebApi.h"
#include "Manager.h"

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace Hook {
#define HOOK_FIELD "hook."

const string kEnable = HOOK_FIELD "enable";
const string kTimeoutSec = HOOK_FIELD "timeoutSec";
const string kOnPublish = HOOK_FIELD "on_publish";
const string kOnPlay = HOOK_FIELD "on_play";
const string kOnFlowReport = HOOK_FIELD "on_flow_report";
const string kOnRtspRealm = HOOK_FIELD "on_rtsp_realm";
const string kOnRtspAuth = HOOK_FIELD "on_rtsp_auth";
const string kOnStreamChanged = HOOK_FIELD "on_stream_changed";
const string kStreamChangedSchemas = HOOK_FIELD "stream_changed_schemas";
const string kOnStreamNotFound = HOOK_FIELD "on_stream_not_found";
const string kOnRecordMp4 = HOOK_FIELD "on_record_mp4";
const string kOnRecordTs = HOOK_FIELD "on_record_ts";
const string kOnShellLogin = HOOK_FIELD "on_shell_login";
const string kOnStreamNoneReader = HOOK_FIELD "on_stream_none_reader";
const string kOnHttpAccess = HOOK_FIELD "on_http_access";
const string kOnServerStarted = HOOK_FIELD "on_server_started";
const string kOnServerExited = HOOK_FIELD "on_server_exited";
const string kOnServerKeepalive = HOOK_FIELD "on_server_keepalive";
const string kOnServerLoad = HOOK_FIELD "on_server_load";
const string kOnServerReport = HOOK_FIELD "on_server_report";
const string kOnSendRtpStopped = HOOK_FIELD "on_send_rtp_stopped";
const string kOnRtpServerTimeout = HOOK_FIELD "on_rtp_server_timeout";
const string kAliveInterval = HOOK_FIELD "alive_interval";
const string kReportInterval = HOOK_FIELD "report_interval";
const string kApiUrl = HOOK_FIELD "api_url";

const string kRetry = HOOK_FIELD "retry";
const string kRetryDelay = HOOK_FIELD "retry_delay";

static onceToken token([]() {
    mINI::Instance()[kEnable] = true;
    mINI::Instance()[kTimeoutSec] = 10;
    // Default hook address is set to empty, using default behavior (e.g. no authentication)
    mINI::Instance()[kOnPublish] = "";
    mINI::Instance()[kOnPlay] = "";
    mINI::Instance()[kOnFlowReport] = "";
    mINI::Instance()[kOnRtspRealm] = "";
    mINI::Instance()[kOnRtspAuth] = "";
    mINI::Instance()[kOnStreamChanged] = "";
    mINI::Instance()[kOnStreamNotFound] = "";
    mINI::Instance()[kOnRecordMp4] = "";
    mINI::Instance()[kOnRecordTs] = "";
    mINI::Instance()[kOnShellLogin] = "";
    mINI::Instance()[kOnStreamNoneReader] = "";
    mINI::Instance()[kOnHttpAccess] = "";
    mINI::Instance()[kOnServerStarted] = "/api/media-server/start";
    mINI::Instance()[kOnServerExited] = "/api/media-server/end";
    mINI::Instance()[kOnServerKeepalive] = "/api/media-server/heartbeat";
    mINI::Instance()[kOnServerLoad] = "/api/media-server/configuration";
    mINI::Instance()[kOnServerReport] = "/api/media-server/channels:update";
    mINI::Instance()[kOnServerKeepalive] = "";
    mINI::Instance()[kOnSendRtpStopped] = "";
    mINI::Instance()[kOnRtpServerTimeout] = "";
    mINI::Instance()[kAliveInterval] = 5.0;
    mINI::Instance()[kReportInterval] = 60.0;
    mINI::Instance()[kRetry] = 1;
    mINI::Instance()[kRetryDelay] = 3.0;
    mINI::Instance()[kStreamChangedSchemas] = "rtsp/rtmp/fmp4/ts/hls/hls.fmp4";
    mINI::Instance()[kApiUrl] = "";
});
} // namespace Hook

namespace Cluster {
#define CLUSTER_FIELD "cluster."
const string kOriginUrl = CLUSTER_FIELD "origin_url";
const string kTimeoutSec = CLUSTER_FIELD "timeout_sec";
const string kRetryCount = CLUSTER_FIELD "retry_count";

static onceToken token([]() {
    mINI::Instance()[kOriginUrl] = "";
    mINI::Instance()[kTimeoutSec] = 15;
    mINI::Instance()[kRetryCount] = 3;
});

} // namespace Cluster

static void parse_http_response(const SockException &ex, const Parser &res, const function<void(const Value &, const string &, bool)> &fun) {
    bool should_retry = true;
    if (ex) {
        auto errStr = StrPrinter << "[network err]:" << ex << endl;
        fun(Json::nullValue, errStr, should_retry);
        return;
    }
    if (res.status() != "200") {
        auto errStr = StrPrinter << "[bad http status code]:" << res.status() << endl;
        fun(Json::nullValue, errStr, should_retry);
        return;
    }
    Value result;
    try {
        stringstream ss(res.content());
        ss >> result;
    } catch (std::exception &ex) {
        auto errStr = StrPrinter << "[parse json failed]:" << ex.what() << endl;
        fun(Json::nullValue, errStr, should_retry);
        return;
    }
#if 0
    auto code = result["code"];

    if (!code.isInt64()) {
        auto errStr = StrPrinter << "[json code]:" << "code not int:" << code << endl;
        fun(Json::nullValue, errStr, should_retry);
        return;
    }
    should_retry = false;
    if (code.asInt64() != 0) {
        auto errStr = StrPrinter << "[auth failed]: code:" << code << " msg:" << result["msg"] << endl;
        fun(Json::nullValue, errStr, should_retry);
        return;
    }
#endif
    try {
        fun(result, "", should_retry);
    } catch (std::exception &ex) {
        auto errStr = StrPrinter << "[do hook invoker failed]:" << ex.what() << endl;
        // If an exception is still thrown, then re-throw the exception
        fun(Json::nullValue, errStr, should_retry);
    }
}

string to_string(const Value &value) {
    return value.toStyledString();
}

string to_string(const HttpArgs &value) {
    return value.make();
}

const char *getContentType(const Value &value) {
    return "application/json";
}

const char *getContentType(const HttpArgs &value) {
    return "application/x-www-form-urlencoded";
}

string getVhost(const Value &value) {
    const char *key = VHOST_KEY;
    auto val = value.find(key, key + sizeof(VHOST_KEY) - 1);
    return val ? val->asString() : "";
}

string getVhost(const HttpArgs &value) {
    auto val = value.find(VHOST_KEY);
    return val != value.end() ? val->second : "";
}

static atomic<uint64_t> s_hook_index { 0 };

void do_http_hook(const string &url, const ArgsType &body, const HeaderType &header, const function<void(const Value &, const string &)> &func, uint32_t retry) {
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    GET_CONFIG(float, hook_timeoutSec, Hook::kTimeoutSec);
    GET_CONFIG(float, retry_delay, Hook::kRetryDelay);

    const_cast<ArgsType &>(body)["mediaServerId"] = mediaServerId;
    const_cast<ArgsType &>(body)["hook_index"] = (Json::UInt64)(s_hook_index++);

    auto requester = std::make_shared<HttpRequester>();
    requester->setMethod("POST");
    auto bodyStr = to_string(body);
    requester->setBody(bodyStr);
    for (const auto &it : header) {
        requester->addHeader(it.first, it.second);
    }
    requester->addHeader("Content-Type", getContentType(body));
    auto vhost = getVhost(body);
    if (!vhost.empty()) {
        requester->addHeader("X-VHOST", vhost);
    }
    Ticker ticker;
    requester->startRequester(url, [url, func, bodyStr, body, header, requester, ticker, retry](const SockException &ex, const Parser &res) mutable {
        onceToken token(nullptr, [&]() mutable { requester.reset(); });
        parse_http_response(ex, res, [&](const Value &obj, const string &err, bool should_retry) {
            if (!err.empty()) {
                // Hook failed
                WarnL << "hook " << url << " " << ticker.elapsedTime() << "ms,failed" << err << ":" << bodyStr;

                if (retry-- > 0 && should_retry) {
                    requester->getPoller()->doDelayTask(MAX(retry_delay, 0.0) * 1000, [url, body, header, func, retry] {
                        do_http_hook(url, body, header, func, retry);
                        return 0;
                    });
                    // Retry does not need to trigger callback
                    return;
                }

            } else if (ticker.elapsedTime() > 500) {
                // Hook succeeded, but hook response exceeded 500ms, print warning log
                DebugL << "hook " << url << " " << ticker.elapsedTime() << "ms,success:" << bodyStr;
            }

            if (func) {
                func(obj, err);
            }
        });
    }, hook_timeoutSec);
}

void do_http_hook(const string &url, const ArgsType &body, const function<void(const Value &, const string &)> &func) {
    GET_CONFIG(uint32_t, hook_retry, Hook::kRetry);
    HeaderType header;
    do_http_hook(url, body, header, func, hook_retry);
}

void do_http_hook(const string &url, const ArgsType &body, const HeaderType &header, const function<void(const Value &, const string &)> &func) {
    GET_CONFIG(uint32_t, hook_retry, Hook::kRetry);
    do_http_hook(url, body, header, func, hook_retry);
}

void do_http_hook(const string &url, const HttpArgs &param, const HeaderType &header, const function<void(const Value &, const string &)> &func, uint32_t retry) {
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    GET_CONFIG(float, hook_timeoutSec, Hook::kTimeoutSec);
    GET_CONFIG(float, retry_delay, Hook::kRetryDelay);

    const_cast<HttpArgs &>(param)["mediaServerId"] = mediaServerId;
    const_cast<HttpArgs &>(param)["hook_index"] = (Json::UInt64)(s_hook_index++);

    auto requester = std::make_shared<HttpRequester>();
    requester->setMethod("GET");
    auto paramStr = to_string(param);
    auto full_url = url + "?" + paramStr;
    for (const auto &it : header) {
        requester->addHeader(it.first, it.second);
    }
    requester->addHeader("Content-Type", getContentType(param));
    auto vhost = getVhost(param);
    if (!vhost.empty()) {
        requester->addHeader("X-VHOST", vhost);
    }
    Ticker ticker;
    requester->startRequester(full_url, [url, func, paramStr, param, header, requester, ticker, retry](const SockException &ex, const Parser &res) mutable {
        onceToken token(nullptr, [&]() mutable { requester.reset(); });
        parse_http_response(ex, res, [&](const Value &obj, const string &err, bool should_retry) {
            if (!err.empty()) {
                // Hook failed
                WarnL << "hook " << url << " " << ticker.elapsedTime() << "ms,failed" << err << ":" << paramStr;

                if (retry-- > 0 && should_retry) {
                    requester->getPoller()->doDelayTask(MAX(retry_delay, 0.0) * 1000, [url, param, header, func, retry] {
                        do_http_hook(url, param, header, func, retry);
                        return 0;
                    });
                    // Retry does not need to trigger callback
                    return;
                }

            } else if (ticker.elapsedTime() > 500) {
                // Hook succeeded, but hook response exceeded 500ms, print warning log
                DebugL << "hook " << url << " " << ticker.elapsedTime() << "ms,success:" << paramStr;
            }

            if (func) {
                func(obj, err);
            }
        });
    }, hook_timeoutSec);
}

void do_http_hook(const std::string &url, const HttpArgs &param, const function<void(const Json::Value &, const string &)> &func) {
    GET_CONFIG(uint32_t, hook_retry, Hook::kRetry);
    HeaderType header;
    do_http_hook(url, param, header, func, hook_retry);
}

void do_http_hook(const std::string &url, const HttpArgs &param, const HeaderType &header, const function<void(const Json::Value &, const string &)> &func) {
    GET_CONFIG(uint32_t, hook_retry, Hook::kRetry);
    do_http_hook(url, param, header, func, hook_retry);
}

void dumpMediaTuple(const MediaTuple &tuple, Json::Value& item);

static ArgsType make_json(const MediaInfo &args) {
    ArgsType body;
    body["schema"] = args.schema;
    if(!args.protocol.empty()){
        body["protocol"] = args.protocol;
    }else{
        body["protocol"] = args.schema;
    }
    dumpMediaTuple(args, body);
    body["params"] = args.params;
    return body;
}

static void reportServerStarted() {
    GET_CONFIG(bool, hook_enable, Hook::kEnable);
    GET_CONFIG(string, hook_server_started, Hook::kOnServerStarted);
    GET_CONFIG(string, hook_api_url, Hook::kApiUrl);
    if (!hook_enable || hook_server_started.empty() || hook_api_url.empty()) {
        return;
    }
    ArgsType body;
#if 0    
    for (auto &pr : mINI::Instance()) {
        body[pr.first] = (string &)pr.second;
    }
#endif
    GET_CONFIG(string, mediaServerId, General::kMediaServerId)
    GET_CONFIG(string, mediaServerDomain, Manager::kMediaServerDomain)
    body["id"] = mediaServerId;
    body["domain"] = mediaServerDomain;
    body["ip"] = GlobalMonitor::Instance().getLocalIps();
    body["macAddress"] = GlobalMonitor::Instance().getMacAddresses();
    body["rtspPort"] = mINI::Instance()["rtsp.port"];
    body["rtmpPort"] = mINI::Instance()["rtmp.port"];
    body["httpPort"] = mINI::Instance()["http.port"];
    body["httpsPort"] = mINI::Instance()["http.sslport"];
    // Execute hook
    do_http_hook(hook_api_url + hook_server_started, body, nullptr);
}

static void reportServerExited() {
    GET_CONFIG(bool, hook_enable, Hook::kEnable);
    GET_CONFIG(string, hook_server_exited, Hook::kOnServerExited);
    GET_CONFIG(string, hook_api_url, Hook::kApiUrl);
    if (!hook_enable || hook_server_exited.empty() || hook_api_url.empty()) {
        return;
    }
    ArgsType body;
#if 0  
    for (auto &pr : mINI::Instance()) {
        body[pr.first] = (string &)pr.second;
    }
#endif
    // Execute hook
    do_http_hook(hook_api_url + hook_server_exited, body, nullptr);
}

// Server keep-alive timer
static Timer::Ptr g_keepalive_timer;
static void reportServerKeepalive() {
    GET_CONFIG(bool, hook_enable, Hook::kEnable);
    GET_CONFIG(string, hook_server_keepalive, Hook::kOnServerKeepalive);
    GET_CONFIG(string, hook_api_url, Hook::kApiUrl);
    if (!hook_enable || hook_server_keepalive.empty() || hook_api_url.empty()) {
        return;
    }
    GET_CONFIG(float, alive_interval, Hook::kAliveInterval);
    g_keepalive_timer = std::make_shared<Timer>(alive_interval,[]() {
#if 0 
        getStatisticJson([](const Value &data) mutable {
            ArgsType body;
            body["data"] = data;
            // Execute hook
            do_http_hook(hook_server_keepalive, body, nullptr);
        });
#endif
        ArgsType body;
        // Execute hook
        do_http_hook(hook_api_url + hook_server_keepalive, body, nullptr);
        return true;
    }, nullptr);
}

// Server report statistics
static Timer::Ptr g_report_timer;
static bool report_first = true;
static void reportServerStatistic() {
    GET_CONFIG(bool, hook_enable, Hook::kEnable);
    GET_CONFIG(string, hook_api_url, Hook::kApiUrl);
    GET_CONFIG(string, hook_server_load, Hook::kOnServerLoad);
    GET_CONFIG(string, hook_server_report, Hook::kOnServerReport);
    if (!hook_enable || hook_server_report.empty() || hook_server_load.empty() || hook_api_url.empty()) {
        return;
    }
    GET_CONFIG(float, report_interval, Hook::kReportInterval);

    auto report_callback = []() {
        ArgsType body;
        do_http_hook(hook_api_url + hook_server_load, body, [](const Value &obj, const string &err) mutable {
            if (err.empty()) {
                DebugL << "hook " << hook_api_url + hook_server_load << " success:" << obj.toStyledString();
                InfoL << "Load server config success";
                // Load server config success
                loadServerConfigJson(obj);

                EventPollerPool::Instance().getPoller()->doDelayTask(5000, []() {
                    getServerStatisticJson([](const Value &data) mutable {
                        ArgsType body;
                        body["data"] = data;
                        // Execute hook
                        do_http_hook(hook_api_url + hook_server_report, body, [](const Value &obj, const string &err) mutable {
                            if (err.empty()) {
                                // Report server statistic succeeded
                                DebugL << "hook " << hook_api_url + hook_server_report << " success:" << obj.toStyledString();
                                InfoL << "Report server statistic success";
                            } else {
                                // Load server config failed
                                DebugL << "hook " <<  hook_api_url + hook_server_report << " failed:" << err;
                                WarnL << "Report server statistic failed:" << err;
                            }
                        });
                    });
                    return 0;
                });

            } else {
                // Load server config failed
                DebugL << "hook " << hook_api_url + hook_server_load << " failed:" << err;
                WarnL << "Load server config failed:" << err;
            }
        });
        if (report_first) {
            report_first = false;
            return false;
        }
        return true;
    };

    g_report_timer = std::make_shared<Timer>(report_interval, report_callback, nullptr);

    EventPollerPool::Instance().getPoller()->doDelayTask(10000, report_callback);
}

static const string kEdgeServerParam = "edge=1";

static string getPullUrl(const string &origin_fmt, const MediaInfo &info) {
    char url[1024] = { 0 };
    if ((ssize_t)origin_fmt.size() > snprintf(url, sizeof(url), origin_fmt.data(), info.app.data(), info.stream.data())) {
        WarnL << "get origin url failed, origin_fmt:" << origin_fmt;
        return "";
    }
    // Inform the origin station that this is a pull stream request from the edge station, if the stream is not found, please return the pull stream failure immediately
    return string(url) + '?' + kEdgeServerParam + '&' + VHOST_KEY + '=' + info.vhost + '&' + info.params;
}

static void pullStreamFromOrigin(const vector<string> &urls, size_t index, size_t failed_cnt, const MediaInfo &args, const function<void()> &closePlayer) {
    GET_CONFIG(float, cluster_timeout_sec, Cluster::kTimeoutSec);
    GET_CONFIG(int, retry_count, Cluster::kRetryCount);

    auto url = getPullUrl(urls[index % urls.size()], args);
    auto timeout_sec = cluster_timeout_sec / urls.size();
    InfoL << "pull stream from origin, failed_cnt: " << failed_cnt << ", timeout_sec: " << timeout_sec << ", url: " << url;

    ProtocolOption option;
    option.enable_hls = option.enable_hls || (args.schema == HLS_SCHEMA);
    option.enable_mp4 = false;

    addStreamProxy(args, url, retry_count, option, Rtsp::RTP_TCP, timeout_sec, mINI{}, [=](const SockException &ex, const string &key) mutable {
        if (!ex) {
            return;
        }
        // Pull stream failed
        if (++failed_cnt == urls.size()) {
            // All origin stations have been retried
            WarnL << "pull stream from origin final failed: " << url;
            closePlayer();
            return;
        }
        pullStreamFromOrigin(urls, index + 1, failed_cnt, args, closePlayer);
    });
}

static void *web_hook_tag = nullptr;

static mINI jsonToMini(const Value &obj) {
    mINI ret;
    if (obj.isObject()) {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            try {
                auto str = (*it).asString();
                ret[it.name()] = std::move(str);
            } catch (std::exception &) {
                WarnL << "Json is not convertible to string, key: " << it.name() << ", value: " << (*it);
            }
        }
    }
    return ret;
}

void installWebHook() {
    GET_CONFIG(bool, hook_enable, Hook::kEnable);

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastMediaPublish, [](BroadcastMediaPublishArgs) {
        GET_CONFIG(string, hook_publish, Hook::kOnPublish);
        if (!hook_enable || hook_publish.empty()) {
            invoker("", ProtocolOption());
            return;
        }

        // Asynchronously execute this hook api to prevent blocking NoticeCenter
        auto body = make_json(args);
        body["ip"] = sender.get_peer_ip();
        body["port"] = sender.get_peer_port();
        body["id"] = sender.getIdentifier();
        body["originType"] = (int)type;
        body["originTypeStr"] = getOriginTypeString(type);
        // Execute hook
        do_http_hook(hook_publish, body, [invoker](const Value &obj, const string &err) mutable {
            if (err.empty()) {
                // Push stream authentication succeeded
                invoker(err, ProtocolOption(jsonToMini(obj)));
            } else {
                // Push stream authentication failed
                invoker(err, ProtocolOption());
            }
        });
    });

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastMediaPlayed, [](BroadcastMediaPlayedArgs) {
        GET_CONFIG(string, hook_play, Hook::kOnPlay);
        if (!hook_enable || hook_play.empty()) {
            invoker("");
            return;
        }

        auto body = make_json(args);
        body["ip"] = sender.get_peer_ip();
        body["port"] = sender.get_peer_port();
        body["id"] = sender.getIdentifier();
        // Execute hook
        do_http_hook(hook_play, body, [invoker](const Value &obj, const string &err) { invoker(err); });
    });

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastFlowReport, [](BroadcastFlowReportArgs) {
        GET_CONFIG(string, hook_flowreport, Hook::kOnFlowReport);
        if (!hook_enable || hook_flowreport.empty()) {
            return;
        }

        auto body = make_json(args);
        body["totalBytes"] = (Json::UInt64)totalBytes;
        body["duration"] = (Json::UInt64)totalDuration;
        body["player"] = isPlayer;
        body["ip"] = sender.get_peer_ip();
        body["port"] = sender.get_peer_port();
        body["id"] = sender.getIdentifier();
        // Execute hook
        do_http_hook(hook_flowreport, body, nullptr);
    });

    static const string unAuthedRealm = "unAuthedRealm";

    // Listen to the kBroadcastOnGetRtspRealm event to determine whether the rtsp link needs authentication (traditional rtsp authentication scheme) to access
    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastOnGetRtspRealm, [](BroadcastOnGetRtspRealmArgs) {
        GET_CONFIG(string, hook_rtsp_realm, Hook::kOnRtspRealm);
        if (!hook_enable || hook_rtsp_realm.empty()) {
            // No authentication required
            invoker("");
            return;
        }

        auto body = make_json(args);
        body["ip"] = sender.get_peer_ip();
        body["port"] = sender.get_peer_port();
        body["id"] = sender.getIdentifier();
        // Execute hook
        do_http_hook(hook_rtsp_realm, body, [invoker](const Value &obj, const string &err) {
            if (!err.empty()) {
                // If the interface access fails, then the rtsp stream authentication fails
                invoker(unAuthedRealm);
                return;
            }
            invoker(obj["realm"].asString());
        });
    });

    // Listen to the kBroadcastOnRtspAuth event to return the correct rtsp authentication username and password
    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastOnRtspAuth, [](BroadcastOnRtspAuthArgs) {
        GET_CONFIG(string, hook_rtsp_auth, Hook::kOnRtspAuth);
        if (unAuthedRealm == realm || !hook_enable || hook_rtsp_auth.empty()) {
            // Authentication failed
            invoker(false, makeRandStr(12));
            return;
        }

        auto body = make_json(args);
        body["ip"] = sender.get_peer_ip();
        body["port"] = sender.get_peer_port();
        body["id"] = sender.getIdentifier();
        body["user_name"] = user_name;
        body["must_no_encrypt"] = must_no_encrypt;
        body["realm"] = realm;
        // Execute hook
        do_http_hook(hook_rtsp_auth, body, [invoker](const Value &obj, const string &err) {
            if (!err.empty()) {
                // Authentication failed
                invoker(false, makeRandStr(12));
                return;
            }
            invoker(obj["encrypted"].asBool(), obj["passwd"].asString());
        });
    });

    // Listen to rtsp, rtmp source registration or deregistration events
    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastMediaChanged, [](BroadcastMediaChangedArgs) {
        GET_CONFIG(string, hook_stream_changed, Hook::kOnStreamChanged);
        if (!hook_enable || hook_stream_changed.empty()) {
            return;
        }

        GET_CONFIG_FUNC(std::set<std::string>, stream_changed_set, Hook::kStreamChangedSchemas, [](const std::string &str) {
            std::set<std::string> ret;
            auto vec = split(str, "/");
            for (auto &schema : vec) {
                trim(schema);
                if (!schema.empty()) {
                    ret.emplace(schema);
                }
            }
            return ret;
        });
        if (!stream_changed_set.empty() && stream_changed_set.find(sender.getSchema()) == stream_changed_set.end()) {
            // This protocol registration deregistration event is ignored
            return;
        }

        ArgsType body;
        if (bRegist) {
            body = makeMediaSourceJson(sender);
            body["regist"] = bRegist;
        } else {
            body["schema"] = sender.getSchema();
            dumpMediaTuple(sender.getMediaTuple(), body);
            body["regist"] = bRegist;
        }
        // Execute hook
        do_http_hook(hook_stream_changed, body, nullptr);
    });

    GET_CONFIG_FUNC(vector<string>, origin_urls, Cluster::kOriginUrl, [](const string &str) {
        vector<string> ret;
        for (auto &url : split(str, ";")) {
            trim(url);
            if (!url.empty()) {
                ret.emplace_back(url);
            }
        }
        return ret;
    });

    // Listen to playback failure (specific stream not found) event
    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastNotFoundStream, [](BroadcastNotFoundStreamArgs) {
        if (!origin_urls.empty()) {
            // If the source station is set, then try to trace the source
            static atomic<uint8_t> s_index { 0 };
            pullStreamFromOrigin(origin_urls, s_index.load(), 0, args, closePlayer);
            ++s_index;
            return;
        }

        if (start_with(args.params, kEdgeServerParam)) {
            // The source station receives a trace request from the edge station, and immediately returns a pull stream failure if the stream does not exist
            closePlayer();
            return;
        }

        GET_CONFIG(string, hook_stream_not_found, Hook::kOnStreamNotFound);
        if (!hook_enable || hook_stream_not_found.empty()) {
            return;
        }

        auto body = make_json(args);
        body["ip"] = sender.get_peer_ip();
        body["port"] = sender.get_peer_port();
        body["id"] = sender.getIdentifier();

        // Hook reply immediately closes the stream
        auto res_cb = [closePlayer](const Value &res, const string &err) {
            bool flag = res["close"].asBool();
            if (flag) {
                closePlayer();
            }
        };

        // Execute hook
        do_http_hook(hook_stream_not_found, body, res_cb);
    });

    static auto getRecordInfo = [](const RecordInfo &info) {
        ArgsType body;
        body["start_time"] = (Json::UInt64)info.start_time;
        body["file_size"] = (Json::UInt64)info.file_size;
        body["time_len"] = info.time_len;
        body["file_path"] = info.file_path;
        body["file_name"] = info.file_name;
        body["folder"] = info.folder;
        body["url"] = info.url;
        dumpMediaTuple(info, body);
        return body;
    };

#ifdef ENABLE_MP4
    // Broadcast after recording the mp4 file successfully
    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        GET_CONFIG(string, hook_record_mp4, Hook::kOnRecordMp4);
        if (!hook_enable || hook_record_mp4.empty()) {
            return;
        }

        // Execute hook
        do_http_hook(hook_record_mp4, getRecordInfo(info), nullptr);
    });
#endif // ENABLE_MP4

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastRecordTs, [](BroadcastRecordTsArgs) {
        GET_CONFIG(string, hook_record_ts, Hook::kOnRecordTs);
        if (!hook_enable || hook_record_ts.empty()) {
            return;
        }

        // Execute hook
        do_http_hook(hook_record_ts, getRecordInfo(info), nullptr);
    });

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastShellLogin, [](BroadcastShellLoginArgs) {
        GET_CONFIG(string, hook_shell_login, Hook::kOnShellLogin);
        if (!hook_enable || hook_shell_login.empty()) {
            invoker("");
            return;
        }

        ArgsType body;
        body["ip"] = sender.get_peer_ip();
        body["port"] = sender.get_peer_port();
        body["id"] = sender.getIdentifier();
        body["user_name"] = user_name;
        body["passwd"] = passwd;

        // Execute hook
        do_http_hook(hook_shell_login, body, [invoker](const Value &, const string &err) { invoker(err); });
    });

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastStreamNoneReader, [](BroadcastStreamNoneReaderArgs) {
        if (!origin_urls.empty() && sender.getOriginType() == MediaOriginType::pull) {
            // If no one is watching at the edge station, stop tracing immediately if it is pulling
            sender.close(false);
            WarnL << "Actively close stream without watching:" << sender.getOriginUrl();
            return;
        }
        GET_CONFIG(string, hook_stream_none_reader, Hook::kOnStreamNoneReader);
        if (!hook_enable || hook_stream_none_reader.empty()) {
            return;
        }

        ArgsType body;
        body["schema"] = sender.getSchema();
        dumpMediaTuple(sender.getMediaTuple(), body);
        weak_ptr<MediaSource> weakSrc = sender.shared_from_this();
        // Execute hook
        do_http_hook(hook_stream_none_reader, body, [weakSrc](const Value &obj, const string &err) {
            bool flag = obj["close"].asBool();
            auto strongSrc = weakSrc.lock();
            if (!flag || !err.empty() || !strongSrc) {
                return;
            }
            strongSrc->close(false);
            WarnL << "Actively close stream without watching:" << strongSrc->getOriginUrl();
        });
    });

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastSendRtpStopped, [](BroadcastSendRtpStoppedArgs) {
        GET_CONFIG(string, hook_send_rtp_stopped, Hook::kOnSendRtpStopped);
        if (!hook_enable || hook_send_rtp_stopped.empty()) {
            return;
        }

        ArgsType body;
        dumpMediaTuple(sender.getMediaTuple(), body);
        body["ssrc"] = ssrc;
        body["originType"] = (int)sender.getOriginType(MediaSource::NullMediaSource());
        body["originTypeStr"] = getOriginTypeString(sender.getOriginType(MediaSource::NullMediaSource()));
        body["originUrl"] = sender.getOriginUrl(MediaSource::NullMediaSource());
        body["msg"] = ex.what();
        body["err"] = ex.getErrCode();
        // Execute hook
        do_http_hook(hook_send_rtp_stopped, body, nullptr);
    });

    /**
     * kBroadcastHttpAccess event trigger mechanism
     * 1. Find the cookie according to the http request header, and find it to enter step 3
     * 2. Find the cookie according to the http url parameter, if the cookie is still not found, enter step 5
     * 3. The cookie marks whether it has permission to access the file, if it has permission, return the file directly
     * 4. Whether the url parameter recorded in the cookie is consistent with the current url parameter, if it is consistent, return the client error code directly
     * 5. Trigger the kBroadcastHttpAccess event
     */
    // Developers should use this event to determine whether the http client has permission to access specific files on the http server
    // S3MediaKit will record the result of this authentication to the cookie
    // If the authentication is successful, within the validity period of the cookie, the next time the client accesses the authorized directory, S3MediaKit will return the file directly
    // If the authentication fails, within the validity period of the cookie, if the http url parameter remains unchanged (otherwise the authentication event will be triggered immediately), S3MediaKit will return the error code directly
    // If the user client does not support cookies, then S3MediaKit will find the cookie according to the url parameter and track the user,
    // If there is no url parameter and the client does not support cookies, then the user will be tracked according to the ip and port
    // The purpose of tracking users is to cache the last authentication result, reduce the number of authentication times, and improve performance
    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastHttpAccess, [](BroadcastHttpAccessArgs) {
        GET_CONFIG(string, hook_http_access, Hook::kOnHttpAccess);
        if (!hook_enable || hook_http_access.empty()) {
            // If http file access authentication is not enabled, then access is allowed, but authentication is required for each access;
            // Because authentication may be enabled at any time in the future (authentication may be re-enabled after reloading the configuration file)
            if (!HttpFileManager::isIPAllowed(sender.get_peer_ip())) {
                invoker("Your ip is not allowed to access the service.", "", 0);
            } else {
                invoker("", "", 0);
            }
            return;
        }

        ArgsType body;
        body["ip"] = sender.get_peer_ip();
        body["port"] = sender.get_peer_port();
        body["id"] = sender.getIdentifier();
        body["path"] = path;
        body["is_dir"] = is_dir;
        body["params"] = parser.params();
        for (auto &pr : parser.getHeader()) {
            body[string("header.") + pr.first] = pr.second;
        }
        // Execute hook
        do_http_hook(hook_http_access, body, [invoker](const Value &obj, const string &err) {
            if (!err.empty()) {
                // If the interface access fails, then only this time does not have permission to access the http server
                invoker(err, "", 0);
                return;
            }
            // The err parameter represents the reason why it cannot be accessed, empty means it can be accessed
            // The path parameter is the top directory that this client can access or is prohibited, if path is an empty string, it means the current directory
            // The second parameter specifies the timeout time of this cookie, if second is 0, the result of this authentication will not be cached
            invoker(obj["err"].asString(), obj["path"].asString(), obj["second"].asInt());
        });
    });

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastRtpServerTimeout, [](BroadcastRtpServerTimeoutArgs) {
        GET_CONFIG(string, rtp_server_timeout, Hook::kOnRtpServerTimeout);
        if (!hook_enable || rtp_server_timeout.empty()) {
            return;
        }

        ArgsType body;
        body["local_port"] = local_port;
        body[VHOST_KEY] = tuple.vhost;
        body["app"] = tuple.app;
        body["stream_id"] = tuple.stream;
        body["tcp_mode"] = tcp_mode;
        body["re_use_port"] = re_use_port;
        body["ssrc"] = ssrc;
        do_http_hook(rtp_server_timeout, body);
    });

    NoticeCenter::Instance().addListener(&web_hook_tag, Broadcast::kBroadcastRestartServer, [](BroadcastRestartServerArgs) {
        GET_CONFIG(string, secret_key, API::kSecret);
        GET_CONFIG(uint16_t, http_port, "http.port");

        ostringstream ss;
        ss << "http://localhost";
        if (http_port > 0 && http_port != 80) {
            ss << ":" << http_port;
        }
        ss << "/index/api/restartServer";
        string restart_server_trigger = ss.str();

        HttpArgs params;
        params["secret"] = secret_key;
        
        do_http_hook(restart_server_trigger, params);
    });

    // Report server restart
    reportServerStarted();

    // Report keep-alive regularly
    reportServerKeepalive();

    // Report server statistics
    reportServerStatistic();
}

void unInstallWebHook() {
    g_keepalive_timer.reset();
    g_report_timer.reset();
    NoticeCenter::Instance().delListener(&web_hook_tag);
}

void onProcessExited() {
    reportServerExited();
}
