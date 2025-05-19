#include <sstream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Http/HttpSession.h"
#include "Http/HttpRequester.h"
#include "Network/Session.h"
#include "Record/Recorder.h"
#include "../server/WebHook.h"
#include "../server/WebApi.h"
#include "config.h"
#include "ManagerHook.h"
#include "Local/TimePeriodRecorder.h"

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace xHook { 
#define XHOOK_FIELD "xHook."
const string kEnable = XHOOK_FIELD "enable";
const string kApiUrl = XHOOK_FIELD "api_url";
const string kTimeoutSec = XHOOK_FIELD "timeoutSec";
const string kOnResourceChanged = XHOOK_FIELD "on_resource_changed";
const string kOnResourceNotFound = XHOOK_FIELD "on_resource_not_found";
const string kOnResourceNoneReader = XHOOK_FIELD "on_resource_none_reader";
const string kOnServerStarted = XHOOK_FIELD "on_server_started";
const string kOnServerExited = XHOOK_FIELD "on_server_exited";
const string kOnServerKeepalive = XHOOK_FIELD "on_server_keepalive";
const string kOnServerLoad = XHOOK_FIELD "on_server_load";
const string kOnServerReport = XHOOK_FIELD "on_server_report";
const string kAliveInterval = XHOOK_FIELD "alive_interval";
const string kReportInterval = XHOOK_FIELD "report_interval";
const string kRetry = XHOOK_FIELD "retry";
const string kRetryDelay = XHOOK_FIELD "retry_delay";

static onceToken token([]() {
    mINI::Instance()[kEnable] = false;
    mINI::Instance()[kTimeoutSec] = 10;
    mINI::Instance()[kOnResourceChanged] = "";
    mINI::Instance()[kOnResourceNotFound] = "";
    mINI::Instance()[kOnServerStarted] = "";
    mINI::Instance()[kOnServerExited] = "";
    mINI::Instance()[kOnServerKeepalive] = "";
    mINI::Instance()[kOnServerLoad] = "";
    mINI::Instance()[kOnServerReport] = "";
    mINI::Instance()[kAliveInterval] = 5.0;
    mINI::Instance()[kReportInterval] = 60.0;
    mINI::Instance()[kRetry] = 1;
    mINI::Instance()[kRetryDelay] = 3.0;
});
} // namespace xHook

// static void parse_http_response(const SockException &ex, const Parser &res, const function<void(const Value &, const string &, bool)> &fun) {
//     bool should_retry = true;
//     if (ex) {
//         auto errStr = StrPrinter << "[network err]:" << ex << endl;
//         fun(Json::nullValue, errStr, should_retry);
//         return;
//     }
//     if (res.status() != "200") {
//         auto errStr = StrPrinter << "[bad http status code]:" << res.status() << endl;
//         fun(Json::nullValue, errStr, should_retry);
//         return;
//     }
//     Value result;
//     try {
//         stringstream ss(res.content());
//         ss >> result;
//     } catch (std::exception &ex) {
//         auto errStr = StrPrinter << "[parse json failed]:" << ex.what() << endl;
//         fun(Json::nullValue, errStr, should_retry);
//         return;
//     }
//     should_retry = false;
//     try {
//         fun(result, "", should_retry);
//     } catch (std::exception &ex) {
//         auto errStr = StrPrinter << "[do hook invoker failed]:" << ex.what() << endl;
//         // If an exception is still thrown, then re-throw the exception
//         fun(Json::nullValue, errStr, should_retry);
//     }
// }

static void reportServerStarted() { 
    GET_CONFIG(bool, hook_enable, xHook::kEnable);
    GET_CONFIG(string, hook_api_url, xHook::kApiUrl);
    GET_CONFIG(string, hook_server_started, xHook::kOnServerStarted);
    if (!hook_enable || hook_server_started.empty() || hook_api_url.empty()) {
        return;
    }
    ArgsType body;
    // todo:
    // for (auto &pr : mINI::Instance()) {
    //     body[pr.first] = (string &)pr.second;
    // }
    // Execute hook
    do_http_hook(hook_api_url + hook_server_started, body, nullptr);
}

static void reportServerExited() {
    GET_CONFIG(bool, hook_enable, xHook::kEnable);
    GET_CONFIG(string, hook_api_url, xHook::kApiUrl);
    GET_CONFIG(string, hook_server_exited, xHook::kOnServerExited);
    if (!hook_enable || hook_server_exited.empty() || hook_api_url.empty()) {
        return;
    }
    ArgsType body;
    // todo:
    // for (auto &pr : mINI::Instance()) {
    //     body[pr.first] = (string &)pr.second;
    // }
    // Execute hook
    do_http_hook(hook_api_url + hook_server_exited, body, nullptr);
}

// Server keep-alive timer
static Timer::Ptr g_keepalive_timer;
static void reportServerKeepalive() {
    GET_CONFIG(bool, hook_enable, xHook::kEnable);
    GET_CONFIG(string, hook_api_url, xHook::kApiUrl);
    GET_CONFIG(string, hook_server_keepalive, xHook::kOnServerKeepalive);
    if (!hook_enable || hook_server_keepalive.empty() || hook_api_url.empty()) {
        return;
    }
    GET_CONFIG(float, alive_interval, xHook::kAliveInterval);
    g_keepalive_timer = std::make_shared<Timer>(alive_interval,[]() {
        ArgsType body;
        // Execute hook
        do_http_hook(hook_api_url + hook_server_keepalive, body, nullptr);
        return true;
    }, nullptr);
}

static void handleServerResourceJson(Json::Value &data) {
    if (data.isMember("mediaServer")) {

    }

    if (data.isMember("devices") && data["devices"].isArray()) {
        for (auto &dev : data["devices"]) {
            auto device_json = dev;
            addCameraResource(device_json, [](const string &camera_id, const string &stream_id, const string &url) {
                auto tuple = MediaTuple { DEFAULT_VHOST, camera_id, stream_id, "" };
                mINI args;
                args["vhost"] = DEFAULT_VHOST;
                args["app"] = tuple.app;
                args["stream_id"] = tuple.stream;
    
                ProtocolOption option;
    
                std::cout << "Add stream proxy: "<< tuple.app << "/" << tuple.stream << " " << url << std::endl;
                addStreamProxy(tuple, url, 0, option, 0, 10.0, args, [](const SockException &ex, const string &key) {
                    if (ex) {
                        WarnL << "Add stream failed: " << ex.what();
                    } else {
                        InfoL << "Add stream success: " << key;
                    }
                });
            });
        }
    }

    // todo: delCameraResource
}

// Server report statistics
static Timer::Ptr g_report_timer;
static bool report_first = true;
static void reportServerStatistic() {
    GET_CONFIG(bool, hook_enable, xHook::kEnable);
    GET_CONFIG(string, hook_api_url, xHook::kApiUrl);
    GET_CONFIG(string, hook_server_load, xHook::kOnServerLoad);
    GET_CONFIG(string, hook_server_report, xHook::kOnServerReport);
    if (!hook_enable || hook_server_report.empty() || hook_server_load.empty() || hook_api_url.empty()) {
        return;
    }
    GET_CONFIG(float, report_interval, xHook::kReportInterval);

    auto report_callback = []() {
#if 0
        ArgsType body;
        do_http_hook(hook_api_url + hook_server_load, body, [](const Value &obj, const string &err) mutable {
            if (err.empty()) {
                InfoL << "hook " << hook_api_url + hook_server_load << " success:" << obj.toStyledString();
                // Load server config succeeded
                handleServerResourceJson(obj);

                EventPollerPool::Instance().getPoller()->doDelayTask(5000, []() {
                    getServerStatisticJson([](const Value &data) mutable {
                        ArgsType body;
                        body["data"] = data;
                        // Execute hook
                        do_http_hook(hook_api_url + hook_server_report,  [](const Value &obj, const string &err) mutable {
                            if (err.empty()) {
                                // Report server statistic succeeded
                                InfoL << "hook " << hook_api_url + hook_server_report << " success:" << obj.toStyledString();
                            } else {
                                // Load server config failed
                                WarnL << "hook " <<  hook_api_url + hook_server_report << " failed:" << err;
                            }
                        });
                    });
                    return 0;
                });

            } else {
                // Load server config failed
                WarnL << "hook " << hook_api_url + hook_server_load << " failed:" << err;
            }
        });
#else
        Json::Value data;
        data["devices"] = Json::arrayValue;
        Json::Value device;
        device["camera_id"] = "5abab589-88ec-450a-9096-e68fcbfa84fb";
        device["username"] = "admin";
        device["password"] = "Haiphong2025";
        device["manufacturer"] = "Hikivision";
        device["model"] = "DS-2CD2347G1-L";
        device["enable"] = true;
        device["address"] = "27.72.173.71";
        device["httpPort"] = 80;
        device["mediaPort"] = 5555;
        device["streams"] = Json::arrayValue;
        Json::Value channel_1;
        channel_1["channel_id"] = "0aa9322f-c0a3-4518-8273-8a7df3d35ede";
        channel_1["source_url"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile2/media.smp";
        device["streams"].append(channel_1);
        Json::Value channel_2;
        channel_1["channel_id"] = "56c14e52-e578-40c3-8b50-d7c315a36456";
        channel_1["source_url"] = "rtsp://admin:Haiphong2025@27.72.173.71:5555/profile4/media.smp";
        device["streams"].append(channel_2);

        data["devices"].append(device);
        // handleServerResourceJson(data);

#endif
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

// static string getPullUrl(const string &origin_fmt, const ResourceInfo &info) {
    // todo:
    // char url[1024] = { 0 };
    // if ((ssize_t)origin_fmt.size() > snprintf(url, sizeof(url), origin_fmt.data(), info.app.data(), info.stream.data())) {
    //     WarnL << "get origin url failed, origin_fmt:" << origin_fmt;
    //     return "";
    // }
    // // Inform the origin station that this is a pull stream request from the edge station, if the stream is not found, please return the pull stream failure immediately
    // return string(url) + '?' + kEdgeServerParam + '&' + VHOST_KEY + '=' + info.vhost + '&' + info.params;
    // return "";
// }

static void pullResourceFromOrigin(const vector<string> &urls, size_t index, size_t failed_cnt, const MediaInfo &args, const function<void()> &closePlayer) {
    // todo:
    // GET_CONFIG(float, cluster_timeout_sec, Cluster::kTimeoutSec);
    // GET_CONFIG(int, retry_count, Cluster::kRetryCount);

    // auto url = getPullUrl(urls[index % urls.size()], args);
    // auto timeout_sec = cluster_timeout_sec / urls.size();
    // InfoL << "pull stream from origin, failed_cnt: " << failed_cnt << ", timeout_sec: " << timeout_sec << ", url: " << url;

    // ProtocolOption option;
    // option.enable_hls = option.enable_hls || (args.schema == HLS_SCHEMA);
    // option.enable_mp4 = false;

    // addStreamProxy(args, url, retry_count, option, Rtsp::RTP_TCP, timeout_sec, mINI{}, [=](const SockException &ex, const string &key) mutable {
    //     if (!ex) {
    //         return;
    //     }
    //     // Pull stream failed
    //     if (++failed_cnt == urls.size()) {
    //         // All origin stations have been retried
    //         WarnL << "pull stream from origin final failed: " << url;
    //         closePlayer();
    //         return;
    //     }
    //     pullStreamFromOrigin(urls, index + 1, failed_cnt, args, closePlayer);
    // });
}

static void *x_hook_tag = nullptr;

void installxHook() {
    GET_CONFIG(bool, hook_enable, xHook::kEnable);

    NoticeCenter::Instance().addListener(&x_hook_tag, Broadcast::kBroadcastMediaPublish, [](BroadcastMediaPublishArgs) {
        WarnL << "kBroadcastMediaPublish ";
        // todo:
    });

    // Listen to playing rtsp/rtmp/http-flv events. Control playback authentication through this event
    NoticeCenter::Instance().addListener(&x_hook_tag, Broadcast::kBroadcastMediaPlayed, [](BroadcastMediaPlayedArgs) {
        WarnL << "kBroadcastMediaPlayed ";
        // todo:
    });
    
#ifdef ENABLE_MP4
    // Broadcast after recording the mp4 file successfully
    NoticeCenter::Instance().addListener(&x_hook_tag, Broadcast::kBroadcastRecordMP4, [=](BroadcastRecordMP4Args) {
        WarnL << "Record mp4 file " << info.app << " " << info.start_time << " " << info.time_len << " " << info.url;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(info.time_len);
        block.set_file_path(info.file_path);
        
        TimeBlockWriter::Instance().addBlock(block);
    });
#endif // ENABLE_MP4

    // Listen to rtsp, rtmp source registration or deregistration events
    // NoticeCenter::Instance().addListener(&x_hook_tag, xBroadcast::kBroadcastResourceChanged, [](BroadcastResourceChangedArgs) {
    //     WarnL << "kBroadcastResourceChanged ";
        // GET_CONFIG(string, hook_api_url, xHook::kApiUrl);
        // GET_CONFIG(string, hook_resource_changed, xHook::kOnResourceChanged);
        // if (!hook_enable || hook_resource_changed.empty() || hook_api_url.empty()) {
        //     return;
        // }

        // GET_CONFIG_FUNC(std::set<std::string>, stream_changed_set, Hook::kStreamChangedSchemas, [](const std::string &str) {
        //     std::set<std::string> ret;
        //     auto vec = split(str, "/");
        //     for (auto &schema : vec) {
        //         trim(schema);
        //         if (!schema.empty()) {
        //             ret.emplace(schema);
        //         }
        //     }
        //     return ret;
        // });
        // if (!stream_changed_set.empty() && stream_changed_set.find(sender.getSchema()) == stream_changed_set.end()) {
        //     // This protocol registration deregistration event is ignored
        //     return;
        // }

        // ArgsType body;
        // if (bRegist) {
        //     body = makeMediaSourceJson(sender);
        //     body["regist"] = bRegist;
        // } else {
        //     body["schema"] = sender.getSchema();
        //     dumpMediaTuple(sender.getMediaTuple(), body);
        //     body["regist"] = bRegist;
        // }
        // // Execute hook
        // do_http_hook(hook_resource_changed, body, nullptr);
    // });

    // GET_CONFIG_FUNC(vector<string>, origin_urls, Cluster::kOriginUrl, [](const string &str) {
    //     vector<string> ret;
    //     for (auto &url : split(str, ";")) {
    //         trim(url);
    //         if (!url.empty()) {
    //             ret.emplace_back(url);
    //         }
    //     }
    //     return ret;
    // });

    // Listen to playback failure (specific stream not found) event
    // NoticeCenter::Instance().addListener(&x_hook_tag, xBroadcast::kBroadcastNotFoundResource, [](BroadcastNotFoundResourceArgs) {
        // if (!origin_urls.empty()) {
        //     // If the source station is set, then try to trace the source
        //     static atomic<uint8_t> s_index { 0 };
        //     pullResourceFromOrigin(origin_urls, s_index.load(), 0, args, closePlayer);
        //     ++s_index;
        //     return;
        // }

        // if (start_with(args.params, kEdgeServerParam)) {
        //     // The source station receives a trace request from the edge station, and immediately returns a pull stream failure if the stream does not exist
        //     closePlayer();
        //     return;
        // }

        // GET_CONFIG(string, hook_api_url, xHook::kApiUrl);
        // GET_CONFIG(string, hook_resource_not_found, xHook::kOnResourceNotFound);
        // if (!hook_enable || hook_resource_not_found.empty() || hook_api_url.empty()) {
        //     return;
        // }

        // auto body = make_json(args);
        // body["ip"] = sender.get_peer_ip();
        // body["port"] = sender.get_peer_port();
        // body["id"] = sender.getIdentifier();

        // // Hook reply immediately closes the stream
        // auto res_cb = [closePlayer](const Value &res, const string &err) {
        //     bool flag = res["close"].asBool();
        //     if (flag) {
        //         closePlayer();
        //     }
        // };

        // // Execute hook
        // do_http_hook(hook_api_url + hook_resource_not_found, body, res_cb);
    // });

    // NoticeCenter::Instance().addListener(&x_hook_tag, xBroadcast::kBroadcastResourceNoneReader, [](BroadcastResourceNoneReaderArgs) {
        // if (!origin_urls.empty() && sender.getOriginType() == MediaOriginType::pull) {
        //     // If no one is watching at the edge station, stop tracing immediately if it is pulling
        //     sender.close(false);
        //     WarnL << "Actively close stream without watching:" << sender.getOriginUrl();
        //     return;
        // }
        // GET_CONFIG(string, hook_api_url, xHook::kApiUrl);
        // GET_CONFIG(string, hook_resource_none_reader, xHook::kOnResourceNoneReader);
        // if (!hook_enable || hook_resource_none_reader.empty() || hook_api_url.empty()) {
        //     return;
        // }

        // ArgsType body;
        // body["schema"] = sender.getSchema();
        // dumpMediaTuple(sender.getMediaTuple(), body);
        // weak_ptr<MediaSource> weakSrc = sender.shared_from_this();
        // // Execute hook
        // do_http_hook(hook_api_url + hook_resource_none_reader, body, [weakSrc](const Value &obj, const string &err) {
        //     bool flag = obj["close"].asBool();
        //     auto strongSrc = weakSrc.lock();
        //     if (!flag || !err.empty() || !strongSrc) {
        //         return;
        //     }
        //     strongSrc->close(false);
        //     WarnL << "Actively close stream without watching:" << strongSrc->getOriginUrl();
        // });
    // });

    // NoticeCenter::Instance().addListener(&x_hook_tag, Broadcast::kBroadcastHttpAccess, [](BroadcastHttpAccessArgs) {
        // GET_CONFIG(string, hook_api_url, xHook::kApiUrl);
        // GET_CONFIG(string, hook_http_access, Hook::kOnHttpAccess);
        // if (!hook_enable || hook_http_access.empty() || hook_api_url.empty()) {
        //     // If http file access authentication is not enabled, then access is allowed, but authentication is required for each access;
        //     // Because authentication may be enabled at any time in the future (authentication may be re-enabled after reloading the configuration file)
        //     if (!HttpFileManager::isIPAllowed(sender.get_peer_ip())) {
        //         invoker("Your ip is not allowed to access the service.", "", 0);
        //     } else {
        //         invoker("", "", 0);
        //     }
        //     return;
        // }

        // ArgsType body;
        // body["ip"] = sender.get_peer_ip();
        // body["port"] = sender.get_peer_port();
        // body["id"] = sender.getIdentifier();
        // body["path"] = path;
        // body["is_dir"] = is_dir;
        // body["params"] = parser.params();
        // for (auto &pr : parser.getHeader()) {
        //     body[string("header.") + pr.first] = pr.second;
        // }
        // // Execute hook
        // do_http_hook(hook_api_url + hook_http_access, body, [invoker](const Value &obj, const string &err) {
        //     if (!err.empty()) {
        //         // If the interface access fails, then only this time does not have permission to access the http server
        //         invoker(err, "", 0);
        //         return;
        //     }
        //     // The err parameter represents the reason why it cannot be accessed, empty means it can be accessed
        //     // The path parameter is the top directory that this client can access or is prohibited, if path is an empty string, it means the current directory
        //     // The second parameter specifies the timeout time of this cookie, if second is 0, the result of this authentication will not be cached
        //     invoker(obj["err"].asString(), obj["path"].asString(), obj["second"].asInt());
        // });
    // });

    // Report server restart
    reportServerStarted();

    // Report keep-alive regularly
    reportServerKeepalive();

    // Report serverserver statistics
    reportServerStatistic();
};

void unInstallxHook() {
    g_keepalive_timer.reset();
    g_report_timer.reset();
    NoticeCenter::Instance().delListener(&x_hook_tag);
};

void onxProcessExited() {
    reportServerExited();
}

void addCameraResource(Value &device, const function<void(const string &camera_id, const string &stream_id, const string &url)> &cb) {
    string camera_id = device["camera_id"].asString();
    string username = device["username"].asString();
    string password = device["password"].asString();
    string ip = device["address"].asString();

    // todo: check config and compare with old config if exist

    if (device.isMember("streams") && device["streams"].isArray()) {
        for (const auto &str: device["streams"]) {
            string stream_id = str["channel_id"].asString();
            string url = str["source_url"].asString();
            WarnL << url;
            cb(camera_id, stream_id, url);
        }
    }
}

void delCameraResource(const string &id) {

}
