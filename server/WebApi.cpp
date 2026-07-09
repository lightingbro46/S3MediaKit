#include <exception>
#include <sys/stat.h>
#include <math.h>
#include <signal.h>

#ifdef _WIN32
#include <io.h>
#include <iostream>
#include <tchar.h>
#endif // _WIN32

#include <functional>
#include <unordered_map>
#include <regex>
#include "Util/MD5.h"
#include "Util/base64.h"
#include "Util/util.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Network/TcpServer.h"
#include "Network/UdpServer.h"
#include "Thread/WorkThreadPool.h"

#ifdef ENABLE_MYSQL
#include "Util/SqlPool.h"
#endif //ENABLE_MYSQL

#ifdef ENABLE_SQLITE
#include "Util/SqlitePool.h"
#endif //ENABLE_SQLITE

#include "WebApi.h"
#include "WebHook.h"

#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Http/HttpSession.h"
#include "Http/HttpRequester.h"
#include "Player/PlayerProxy.h"
#include "Pusher/PusherProxy.h"
#include "Rtp/RtpProcess.h"
#include "User/UserAuthorManager.h"
#include "User/UserAuditLog.h"
#include "Record/MP4Reader.h"

#if defined(ENABLE_RTPPROXY)
#include "Rtp/RtpServer.h"
#endif

#ifdef ENABLE_WEBRTC
#include "../webrtc/WebRtcPlayer.h"
#include "../webrtc/WebRtcPusher.h"
#include "../webrtc/WebRtcEchoTest.h"
#include "../webrtc/WebRtcSignalingPeer.h"
#include "../webrtc/WebRtcSignalingSession.h"
#include "../webrtc/WebRtcProxyPlayer.h"
#include "../webrtc/WebRtcProxyPlayerImp.h"
#endif

#if defined(ENABLE_VERSION)
#include "S3MVersion.h"
#endif

#if defined(ENABLE_VIDEOSTACK) && defined(ENABLE_X264) && defined (ENABLE_FFMPEG)
#include "VideoStack.h"
#endif

#include "Server/GlobalMonitor.h"
#include "Manager.h"
#include "Onvif/Onvif.h"
#include "Onvif/SoapUtil.h"
#include "WebApiErrCode.h"

#include "Storage/TransactionLog.h"
#include "Storage/TransactionPeerAckLog.h"
#include "Storage/MiscData.h"
#include "Storage/StoragePool.h"
#include "Storage/StoragePolicy.h"
#include "Storage/PolicyAssignment.h"
#include "Storage/TieringJob.h"
#include "Storage/StorageTierExtra.h"
#include "Local/TierStorageManager.h"
#include "Server/ClusterManager.h"

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace API {
#define API_FIELD "api."
const string kApiDebug = API_FIELD"apiDebug";
const string kSecret = API_FIELD"secret";
const string kSnapRoot = API_FIELD"snapRoot";
const string kExtractRoot = API_FIELD"extractRoot";
const string kDefaultSnap = API_FIELD"defaultSnap";
const string kDownloadRoot = API_FIELD"downloadRoot";
const string kJsonNotFoundPrefixs = API_FIELD"jsonNotFoundPrefixes";

static onceToken token([]() {
    mINI::Instance()[kApiDebug] = "1";
    mINI::Instance()[kSecret] = "035c73f7-bb6b-4889-a715-d9eb2d1925cc";
    mINI::Instance()[kSnapRoot] = "./www/snap/";
    mINI::Instance()[kExtractRoot] = "./www/extract/";
    mINI::Instance()[kDefaultSnap] = "./www/logo.png";
    mINI::Instance()[kDownloadRoot] = "./www";
});
} // namespace API

using HttpApi = function<void(const Parser &parser, const HttpSession::HttpResponseInvoker &invoker, SockInfo &sender)>;
// http api list
static map<string, HttpApi, StrCaseCompare> s_map_api;

static void responseApi(const Json::Value &res, int status_code, const HttpSession::HttpResponseInvoker &invoker) {
    GET_CONFIG(string, charSet, Http::kCharSet);
    HttpSession::KeyValue headerOut;
    headerOut["Content-Type"] = string("application/json; charset=") + charSet;
    invoker(status_code, headerOut, res.toStyledString());
};

static void responseApi(int code, const string &msg, int status_code, const HttpSession::HttpResponseInvoker &invoker) {
    Json::Value res;
    res["code"] = code;
    res["msg"] = msg;
    responseApi(res, status_code, invoker);
}

static ApiArgsType getAllArgs(const Parser &parser);

static HttpApi toApi(const function<void(API_ARGS_MAP_ASYNC)> &cb) {
    return [cb](const Parser &parser, const HttpSession::HttpResponseInvoker &invoker, SockInfo &sender) {
        GET_CONFIG(string, charSet, Http::kCharSet);
        HttpSession::KeyValue headerOut;
        headerOut["Content-Type"] = string("application/json; charset=") + charSet;

        Json::Value val;
        val["code"] = API::Success;

        // Parse parameters into a map
        auto args = getAllArgs(parser);
        cb(sender, headerOut, ArgsMap(parser, args), val, invoker);
    };
}

static HttpApi toApi(const function<void(API_ARGS_MAP)> &cb) {
    return toApi([cb](API_ARGS_MAP_ASYNC) {
        cb(API_ARGS_VALUE);
        invoker(200, headerOut, val.toStyledString());
    });
}

static HttpApi toApi(const function<void(API_ARGS_JSON_ASYNC)> &cb) {
    return [cb](const Parser &parser, const HttpSession::HttpResponseInvoker &invoker, SockInfo &sender) {
        GET_CONFIG(string, charSet, Http::kCharSet);
        HttpSession::KeyValue headerOut;
        headerOut["Content-Type"] = string("application/json; charset=") + charSet;

        Json::Value val;
        val["code"] = API::Success;

        if (parser["Content-Type"].find("application/json") == string::npos) {
            throw InvalidArgsException("This interface only supports requests in json format");
        }
        // Parse parameters into a JSON object and then process
        Json::Value args;
        Json::Reader reader;
        reader.parse(parser.content(), args);

        cb(sender, headerOut, ArgsJson(parser, args), val, invoker);
    };
}

static HttpApi toApi(const function<void(API_ARGS_JSON)> &cb) {
    return toApi([cb](API_ARGS_JSON_ASYNC) {
        cb(API_ARGS_VALUE);
        invoker(200, headerOut, val.toStyledString());
    });
}

static HttpApi toApi(const function<void(API_ARGS_STRING_ASYNC)> &cb) {
    return [cb](const Parser &parser, const HttpSession::HttpResponseInvoker &invoker, SockInfo &sender) {
        GET_CONFIG(string, charSet, Http::kCharSet);
        HttpSession::KeyValue headerOut;
        headerOut["Content-Type"] = string("application/json; charset=") + charSet;

        Json::Value val;
        val["code"] = API::Success;

        cb(sender, headerOut, ArgsString(parser, (string &)parser.content()), val, invoker);
    };
}

static HttpApi toApi(const function<void(API_ARGS_STRING)> &cb) {
    return toApi([cb](API_ARGS_STRING_ASYNC) {
        cb(API_ARGS_VALUE);
        invoker(200, headerOut, val.toStyledString());
    });
}

void api_regist(const string &api_path, const function<void(API_ARGS_MAP)> &func) {
    s_map_api.emplace(api_path, toApi(func));
}

void api_regist(const string &api_path, const function<void(API_ARGS_MAP_ASYNC)> &func) {
    s_map_api.emplace(api_path, toApi(func));
}

void api_regist(const string &api_path, const function<void(API_ARGS_JSON)> &func) {
    s_map_api.emplace(api_path, toApi(func));
}

void api_regist(const string &api_path, const function<void(API_ARGS_JSON_ASYNC)> &func) {
    s_map_api.emplace(api_path, toApi(func));
}

void api_regist(const string &api_path, const function<void(API_ARGS_STRING)> &func) {
    s_map_api.emplace(api_path, toApi(func));
}

void api_regist(const string &api_path, const function<void(API_ARGS_STRING_ASYNC)> &func) {
    s_map_api.emplace(api_path, toApi(func));
}

// Get URL parameters and content parameters from the HTTP request
static ApiArgsType getAllArgs(const Parser &parser) {
    ApiArgsType allArgs;
    if (parser["Content-Type"].find("application/x-www-form-urlencoded") == 0) {
        auto contentArgs = parser.parseArgs(parser.content());
        for (auto &pr : contentArgs) {
            allArgs[pr.first] = strCoding::UrlDecodeComponent(pr.second);
        }
    } else if (parser["Content-Type"].find("application/json") == 0) {
        try {
            stringstream ss(parser.content());
            Value jsonArgs;
            ss >> jsonArgs;
            auto keys = jsonArgs.getMemberNames();
            for (auto key = keys.begin(); key != keys.end(); ++key) {
                allArgs[*key] = jsonArgs[*key].asString();
            }
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    } else if (!parser["Content-Type"].empty()) {
        WarnL << "invalid Content-Type:" << parser["Content-Type"];
    }

    for (auto &pr : parser.getUrlArgs()) {
        allArgs[pr.first] = pr.second;
    }
    return allArgs;
}

bool checkUserAuthor(const string &resource_id, const string &jwt_token) {
    GET_CONFIG(bool, enable_authorize, Manager::kEnableAuthorize);
    // If authorization is not enabled, directly pass
    if (!enable_authorize) {
        return true;
    }

    // If there is no token, directly reject
    if (jwt_token.empty()) {
        return false;
    }

    // Check authorization cache
    auto permit = UserAuthorManager::Instance().getAuthorCache(resource_id, jwt_token);
    return permit == UserAuthorPermit::ACCEPT;
}

bool checkPermissionCode(UserSessionCache::Ptr &session, const std::string &key) {
    auto ret = session->hasPermissionCode(key);
    if (!ret) {
        std::string key_str(key); 
        auto code = getApiErrCodeWithPermission(key_str);
        auto message = getDefaultMessage(code);
        throw AuthException(message, code);
    }
    return ret;
}

static bool isApiNamespace(const string &url) {
    GET_CONFIG_FUNC(std::vector<std::string>, url_prefixs, API::kJsonNotFoundPrefixs, [](const string &str) -> std::vector<std::string> {
        auto ret = toolkit::split(str, ",");
        return ret;
    });
    if (url_prefixs.empty()) {
        return false;
    }
    for (const auto &prefix : url_prefixs) {
        if (start_with(url, prefix)) {
            return true;
        }
    }
    return false;
}

extern uint64_t getTotalMemUsage();
extern uint64_t getTotalMemBlock();
extern uint64_t getThisThreadMemUsage();
extern uint64_t getThisThreadMemBlock();
extern std::vector<size_t> getBlockTypeSize();
extern uint64_t getTotalMemBlockByType(int type);
extern uint64_t getThisThreadMemBlockByType(int type);

static void *web_api_tag = nullptr;

static inline void addHttpListener() {
    GET_CONFIG(bool, api_debug, API::kApiDebug);
    // Register to listen for the kBroadcastHttpRequest event
    NoticeCenter::Instance().addListener(&web_api_tag, Broadcast::kBroadcastHttpRequest, [](BroadcastHttpRequestArgs) {
        auto it = s_map_api.find(parser.url());
        if (it == s_map_api.end()) {
            if (isApiNamespace(parser.url())) {
                consumed = true;
                auto msg = getDefaultMessage(ApiErrCode::CODE_ENDPOINT_NOT_SUPPORTED);
                auto status_code = getStatusCode(ApiErrCode::CODE_ENDPOINT_NOT_SUPPORTED);
                responseApi(ApiErrCode::CODE_ENDPOINT_NOT_SUPPORTED, msg, status_code, invoker);
            }
            return;
        }
        // This API has been consumed
        consumed = true;

        if (api_debug) {
            auto newInvoker = [invoker, parser](int code, const HttpSession::KeyValue &headerOut, const HttpBody::Ptr &body) {
                // The body is empty by default
                ssize_t size = 0;
                if (body && body->remainSize()) {
                    // If there is a body, get the body size
                    size = body->remainSize();
                }

                LogContextCapture log(getLogger(), toolkit::LDebug, __FILE__, "http api debug", __LINE__);
                log << "\r\n# request:\r\n" << parser.method() << " " << parser.fullUrl() << "\r\n";
                log << "# header:\r\n";

                for (auto &pr : parser.getHeader()) {
                    log << pr.first << " : " << pr.second << "\r\n";
                }

                auto &content = parser.content();
                log << "# content:\r\n" << (content.size() > 4 * 1024 ? content.substr(0, 4 * 1024) : content) << "\r\n";

                if (size > 0 && size < 4 * 1024) {
                    auto response = body->readData(size);
                    log << "# response:\r\n" << response->data() << "\r\n";
                    invoker(code, headerOut, response);
                } else {
                    log << "# response size:" << size << "\r\n";
                    invoker(code, headerOut, body);
                }
            };
            ((HttpSession::HttpResponseInvoker &)invoker) = newInvoker;
        }
        auto helper = static_cast<SocketHelper &>(sender).shared_from_this();
        // The next event loop of this poller thread, executes the http API to prevent the locks that occupy the NoticeCenter
        helper->getPoller()->async([it, parser, invoker, helper]() {
            try {
                it->second(parser, invoker, *helper);
            } catch (ApiRetException &ex) {
                responseApi(ex.code(), ex.what(), ex.status_code(), invoker);
                helper->getPoller()->async([helper, ex]() { helper->shutdown(SockException(Err_shutdown, ex.what())); }, false);
            }
#ifdef ENABLE_MYSQL
            catch (SqlException &ex) {
                responseApi(API::SqlFailed, StrPrinter << "Failed to operate the database:" << ex.what() << ":" << ex.getSql(), 500, invoker);
            }
#endif // ENABLE_MYSQL
#ifdef ENABLE_SQLITE
            catch (SqliteException &ex) {
                responseApi(API::SqlFailed, StrPrinter << "Failed to operate the database:" << ex.what() << ":" << ex.getSql(), 500, invoker);
            }
#endif // ENABLE_SQLITE
            catch (std::exception &ex) {
                responseApi(API::Exception, ex.what(), 500, invoker);
            }
        },false);
    });
}

// Pull stream proxy list
static ServiceController<PlayerProxy> s_player_proxy;

// Pull replay stream proxy list
static ServiceController<PlayerProxy> s_replay_player_proxy;

// Push stream proxy list
static ServiceController<PusherProxy> s_pusher_proxy;

// FFmpeg pull stream proxy list
static ServiceController<FFmpegSource> s_ffmpeg_src;

#if defined(ENABLE_RTPPROXY)
// RTP server list
static ServiceController<RtpServer> s_rtp_server;
#endif

static inline string getPusherKey(const string &schema, const string &vhost, const string &app, const string &stream, const string &dst_url) {
    return schema + "/" + vhost + "/" + app + "/" + stream + "/" + MD5(dst_url).hexdigest();
}

static void fillSockInfo(Value &val, SockInfo *info) {
    val["peer_ip"] = info->get_peer_ip();
    val["peer_port"] = info->get_peer_port();
    val["local_port"] = info->get_local_port();
    val["local_ip"] = info->get_local_ip();
    val["identifier"] = info->getIdentifier();
}

void dumpMediaTuple(const MediaTuple &tuple, Json::Value &item) {
    item[VHOST_KEY] = tuple.vhost;
    item["app"] = tuple.app;
    item["stream"] = tuple.stream;
    item["params"] = tuple.params;
}

Value ToJson(const PusherProxy::Ptr &p) {
    Value item;
    item["url"] = p->getUrl();
    item["status"] = p->getStatus();
    item["liveSecs"] = p->getLiveSecs();
    item["rePublishCount"] = p->getRePublishCount();    
    item["bytesSpeed"] = (Json::UInt64) p->getSendSpeed();
    item["totalBytes"] =(Json::UInt64) p->getSendTotalBytes();

    if (auto src = p->getSrc()) {
        dumpMediaTuple(src->getMediaTuple(), item["src"]);
    }
    return item;
}

Value ToJson(const PlayerProxy::Ptr &p) {
    Value item;
    item["url"] = p->getUrl();
    item["status"] = p->getStatus();
    item["liveSecs"] = p->getLiveSecs();
    item["rePullCount"] = p->getRePullCount();
    item["totalReaderCount"] = p->totalReaderCount();
    item["bytesSpeed"] = (Json::UInt64) p->getRecvSpeed();
    item["totalBytes"] = (Json::UInt64) p->getRecvTotalBytes();

    dumpMediaTuple(p->getMediaTuple(), item["src"]);
    return item;
}

Value makeMediaSourceJson(MediaSource &media) {
    Value item;
    item["schema"] = media.getSchema();
    dumpMediaTuple(media.getMediaTuple(), item);
    item["createStamp"] = (Json::UInt64) media.getCreateStamp();
    item["currentStamp"] = (Json::UInt64) media.getTimeStamp(TrackInvalid);
    item["aliveSecond"] = (Json::UInt64) media.getAliveSecond();
    item["bytesSpeed"] = (Json::UInt64) media.getBytesSpeed();
    item["totalBytes"] = (Json::UInt64) media.getTotalBytes();
    item["readerCount"] = media.readerCount();
    item["totalReaderCount"] = media.totalReaderCount();
    item["originType"] = (int)media.getOriginType();
    item["originTypeStr"] = getOriginTypeString(media.getOriginType());
    item["originUrl"] = media.getOriginUrl();
    item["isRecordingMP4"] = media.isRecording(Recorder::type_mp4);
    item["isRecordingHLS"] = media.isRecording(Recorder::type_hls);
    auto originSock = media.getOriginSock();
    if (originSock) {
        fillSockInfo(item["originSock"], originSock.get());
    } else {
        item["originSock"] = Json::nullValue;
    }

    // getLossRate has thread safety issues; use the getMediaInfo interface to get the packet loss rate; the getMediaList interface will ignore the packet loss rate
    auto current_thread = false;
    try { current_thread = media.getOwnerPoller()->isCurrentThread();} catch (...) {}
    float last_loss = -1;
    for (auto &track : media.getTracks(false)) {
        Value obj;
        auto codec_type = track->getTrackType();
        obj["codec_id"] = track->getCodecId();
        obj["codec_id_name"] = track->getCodecName();
        obj["ready"] = track->ready();
        obj["codec_type"] = codec_type;
        if (current_thread) {
            // RTP push stream has only one statistics, but may have multiple tracks. If you get the interval packet loss rate multiple times in a short time,
            // the second time will get -1
            auto loss = media.getLossRate(codec_type);
            if (loss == -1) {
                loss = last_loss;
            } else {
                last_loss = loss;
            }
            obj["loss"] = loss;
        }
        obj["frames"] = track->getFrames();
        obj["duration"] = track->getDuration();
        switch (codec_type) {
            case TrackAudio: {
                auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
                obj["sample_rate"] = audio_track->getAudioSampleRate();
                obj["channels"] = audio_track->getAudioChannel();
                obj["sample_bit"] = audio_track->getAudioSampleBit();
                break;
            }
            case TrackVideo: {
                auto video_track = dynamic_pointer_cast<VideoTrack>(track);
                obj["width"] = video_track->getVideoWidth();
                obj["height"] = video_track->getVideoHeight();
                obj["key_frames"] = video_track->getVideoKeyFrames();
                int gop_size = video_track->getVideoGopSize();
                int gop_interval_ms = video_track->getVideoGopInterval();
                float fps = video_track->getVideoFps();
                if (fps <= 1 && gop_interval_ms) {
                    fps = gop_size * 1000.0 / gop_interval_ms;
                }
                obj["fps"] = round(fps);
                obj["gop_size"] = gop_size;
                obj["gop_interval_ms"] = gop_interval_ms;
                break;
            }
            default: break;
        }
        item["tracks"].append(obj);
    }
    return item;
}

#if defined(ENABLE_RTPPROXY)
uint16_t openRtpServer(uint16_t local_port, const mediakit::MediaTuple &tuple, int tcp_mode, const string &local_ip, bool re_use_port, uint32_t ssrc, int only_track, bool multiplex) {
    auto key = tuple.shortUrl();
    if (s_rtp_server.find(key)) {
        // To prevent the problem of all permissions being messed up in RtpProcess, duplicate keys are not allowed to be added
        return 0;
    }

    auto server = s_rtp_server.makeWithAction(key, [&](RtpServer::Ptr server) {
        server->start(local_port, local_ip.c_str(), tuple, (RtpServer::TcpMode)tcp_mode, re_use_port, ssrc, only_track, multiplex);
    });
    server->setOnDetach([key](const SockException &ex) {
        // Set RTP timeout removal event
        s_rtp_server.erase(key);
    });

    // Reply JSON
    return server->getPort();
}

#endif

void getStatisticJson(const function<void(Value &val)> &cb) {
    auto obj = std::make_shared<Value>(objectValue);
    auto &val = *obj;
    val["MediaSource"] = (Json::UInt64)(ObjectStatistic<MediaSource>::count());
    val["MultiMediaSourceMuxer"] = (Json::UInt64)(ObjectStatistic<MultiMediaSourceMuxer>::count());

    val["TcpServer"] = (Json::UInt64)(ObjectStatistic<TcpServer>::count());
    val["TcpSession"] = (Json::UInt64)(ObjectStatistic<TcpSession>::count());
    val["UdpServer"] = (Json::UInt64)(ObjectStatistic<UdpServer>::count());
    val["UdpSession"] = (Json::UInt64)(ObjectStatistic<UdpSession>::count());
    val["TcpClient"] = (Json::UInt64)(ObjectStatistic<TcpClient>::count());
    val["Socket"] = (Json::UInt64)(ObjectStatistic<Socket>::count());

    val["FrameImp"] = (Json::UInt64)(ObjectStatistic<FrameImp>::count());
    val["Frame"] = (Json::UInt64)(ObjectStatistic<Frame>::count());

    val["Buffer"] = (Json::UInt64)(ObjectStatistic<Buffer>::count());
    val["BufferRaw"] = (Json::UInt64)(ObjectStatistic<BufferRaw>::count());
    val["BufferLikeString"] = (Json::UInt64)(ObjectStatistic<BufferLikeString>::count());
    val["BufferList"] = (Json::UInt64)(ObjectStatistic<BufferList>::count());

    val["RtpPacket"] = (Json::UInt64)(ObjectStatistic<RtpPacket>::count());
    val["RtmpPacket"] = (Json::UInt64)(ObjectStatistic<RtmpPacket>::count());
#ifdef ENABLE_MEM_DEBUG
    auto bytes = getTotalMemUsage();
    val["totalMemUsage"] = (Json::UInt64)bytes;
    val["totalMemUsageMB"] = (int)(bytes >> 20);
    val["totalMemBlock"] = (Json::UInt64)getTotalMemBlock();
    static auto block_type_size = getBlockTypeSize();
    {
        int i = 0;
        string str;
        size_t last = 0;
        for (auto sz : block_type_size) {
            str.append(to_string(last) + "~" + to_string(sz) + ":" + to_string(getTotalMemBlockByType(i++)) + ";");
            last = sz;
        }
        str.pop_back();
        val["totalMemBlockTypeCount"] = str;
    }

    auto thread_size = EventPollerPool::Instance().getExecutorSize() + WorkThreadPool::Instance().getExecutorSize();
    std::shared_ptr<vector<Value>> thread_mem_info = std::make_shared<vector<Value>>(thread_size);

    shared_ptr<void> finished(nullptr, [thread_mem_info, cb, obj](void *) {
        for (auto &val : *thread_mem_info) {
            (*obj)["threadMem"].append(val);
        }
        // Trigger callback
        cb(*obj);
    });

    auto pos = 0;
    auto lam0 = [&](TaskExecutor &executor) {
        auto &val = (*thread_mem_info)[pos++];
        executor.async([finished, &val]() {
            auto bytes = getThisThreadMemUsage();
            val["threadName"] = getThreadName();
            val["threadMemUsage"] = (Json::UInt64)bytes;
            val["threadMemUsageMB"] = (Json::UInt64)(bytes >> 20);
            val["threadMemBlock"] = (Json::UInt64)getThisThreadMemBlock();
            {
                int i = 0;
                string str;
                size_t last = 0;
                for (auto sz : block_type_size) {
                    str.append(to_string(last) + "~" + to_string(sz) + ":" + to_string(getThisThreadMemBlockByType(i++)) + ";");
                    last = sz;
                }
                str.pop_back();
                val["threadMemBlockTypeCount"] = str;
            }
        });
    };
    auto lam1 = [lam0](const TaskExecutor::Ptr &executor) {
        lam0(*executor);
    };
    EventPollerPool::Instance().for_each(lam1);
    WorkThreadPool::Instance().for_each(lam1);
#else
    cb(*obj);
#endif
}

void addStreamProxy(const MediaTuple &tuple, const string &url, int retry_count,
                    const ProtocolOption &option, int rtp_type, float timeout_sec, const mINI &args,
                    const function<void(const SockException &ex, const string &key)> &cb) {
    auto key = tuple.shortUrl();
    if (s_player_proxy.find(key)) {
        // Already pulling stream
        cb(SockException(Err_other, "This stream already exists"), key);
        return;
    }
    // Add pull stream proxy
    auto player = s_player_proxy.make(key, tuple, option, retry_count);

    // First pass-through copy parameters
    for (auto &pr : args) {
        (*player)[pr.first] = pr.second;
    }

    // Specify RTP over TCP (effective when playing RTSP)
    (*player)[Client::kRtpType] = rtp_type;

    if (timeout_sec > 0.1f) {
        // Play handshake timeout
        (*player)[Client::kTimeoutMS] = timeout_sec * 1000;
    }

    // Start playing. If playback fails or is stopped, it will automatically retry several times, by default it will retry indefinitely
    player->setPlayCallbackOnce([cb, key](const SockException &ex) {
        if (ex) {
            s_player_proxy.erase(key);
        }
        cb(ex, key);
    });

    // The pull stream was actively closed
    player->setOnClose([key](const SockException &ex) {
        s_player_proxy.erase(key);
    });
    player->play(url);
};

void addStreamProxy(const mediakit::MediaTuple &tuple, const ProtocolOption &option, const std::function<void(const string &err, const PlayerProxy::Ptr &player)> &cb) {
    auto key = tuple.shortUrl();
    if (s_player_proxy.find(key)) {
        // Already pulling stream
        cb("This stream already exists", nullptr);
        return;
    }
    // Add pull stream proxy
    auto player = s_player_proxy.make(key, tuple, option);
    cb("", player);
}

void delStreamProxy(const MediaTuple &tuple) {
    auto key = tuple.shortUrl();
    auto player_proxy = s_player_proxy.find(key);
    if (player_proxy) {
        s_player_proxy.erase(key);
    }
}

void addReplayStreamProxy(const mediakit::MediaTuple &tuple, const ProtocolOption &option, const std::function<void(const std::string &err, const PlayerProxy::Ptr &player)> &cb, int retry_count) {
    auto key = tuple.shortUrl();
    if (s_replay_player_proxy.find(key)) {
        // Already pulling stream
        cb("This replay stream already exists", nullptr);
        return;
    }
    // Add pull stream proxy
    auto player = s_replay_player_proxy.make(key, tuple, option, retry_count);
    cb("", player);
}

const PlayerProxy::Ptr getReplayStreamProxy(const std::string &key) {
    auto replay_player_proxy = s_replay_player_proxy.find(key);
    return replay_player_proxy ? replay_player_proxy : nullptr;
}

void delReplayStreamProxy(const std::string &key) {
    auto player_proxy = s_replay_player_proxy.find(key);
    if (player_proxy) {
        s_replay_player_proxy.erase(key);
    }
}

void addFFmpegSource(const std::string &dst_url, const std::function<void(const string &err, const FFmpegSource::Ptr &player)> &cb) {
    auto key = MD5(dst_url).hexdigest();
    if (s_ffmpeg_src.find(key)) {
        // Already pulling stream
        cb("This stream already exists", nullptr);
        return;
    }
    // Add pull ffmpeg source
    auto ffmpeg = s_ffmpeg_src.make(key);
    cb("", ffmpeg);
};

void delFFmpegSource(const std::string &dst_url) {
    auto key = MD5(dst_url).hexdigest();
    auto ffmpeg_src = s_ffmpeg_src.find(key);
    if (ffmpeg_src) {
        s_ffmpeg_src.erase(key);
    }
}

void addStreamPusherProxy(const string &schema,
                          const string &vhost,
                          const string &app,
                          const string &stream,
                          const string &url,
                          int retry_count,
                          int rtp_type,
                          float timeout_sec,
                          const mINI &args,
                          const function<void(const SockException &ex, const string &key)> &cb) {
    auto key = getPusherKey(schema, vhost, app, stream, url);
    auto src = MediaSource::find(schema, vhost, app, stream);
    if (!src) {
        cb(SockException(Err_other, "can not find the source stream"), key);
        return;
    }
    if (s_pusher_proxy.find(key)) {
        // Already pushing stream
        cb(SockException(Err_success), key);
        return;
    }

    // Add push stream proxy
    auto pusher = s_pusher_proxy.make(key, src, retry_count);

    // First pass-through copy parameters
    for (auto &pr : args) {
        (*pusher)[pr.first] = pr.second;
    }

    // Specify RTP over TCP (effective when playing RTSP)
    (*pusher)[Client::kRtpType] = rtp_type;

    if (timeout_sec > 0.1f) {
        // Push stream handshake timeout
        (*pusher)[Client::kTimeoutMS] = timeout_sec * 1000;
    }

    // Start pushing stream. If the push stream fails or is stopped, it will automatically retry several times, by default it will retry indefinitely
    pusher->setPushCallbackOnce([cb, key, url](const SockException &ex) {
        if (ex) {
            WarnL << "Push " << url << " failed, key: " << key << ", err: " << ex;
            s_pusher_proxy.erase(key);
        }
        cb(ex, key);
    });

    // Stream closed actively
    pusher->setOnClose([key, url](const SockException &ex) {
        WarnL << "Push " << url << " failed, key: " << key << ", err: " << ex;
        s_pusher_proxy.erase(key);
    });
    pusher->publish(url);
}

void getThreadsLoad(TaskExecutorGetterImp &getter, API_ARGS_MAP_ASYNC) {
    getter.getExecutorDelay([&getter, invoker, headerOut](const vector<int> &vecDelay) {
        Value val;
        auto vec = getter.getExecutorLoad();
        std::vector<EventPoller::Ptr> pollers;
        getter.for_each([&](const TaskExecutor::Ptr &exe) { pollers.emplace_back(std::static_pointer_cast<EventPoller>(exe)); });
        int i = API::Success;
        for (auto load : vec) {
            Value obj(objectValue);
            obj["load"] = load;
            auto &poller = pollers[i];
            obj["name"] = poller->getThreadName();
            obj["fd_count"] = static_cast<Json::UInt64>(poller->fdCount());
            obj["delay"] = vecDelay[i++];
            val["data"].append(obj);
        }
        val["code"] = API::Success;
        invoker(200, headerOut, val.toStyledString());
    });
}

static std::unordered_map<std::string, StoragePool> apiLoadStoragePoolMap() {
    StoragePoolImp imp;
    std::unordered_map<std::string, StoragePool> ret;
    for (const auto &pool : imp.queryAll()) {
        ret[pool.id] = pool;
    }
    return ret;
}

static bool apiIsRestoreRequiredPool(const std::string &pool_id,
                                     const std::unordered_map<std::string, StoragePool> &pool_map) {
    auto it = pool_map.find(pool_id);
    return it != pool_map.end() && poolTypeIsObjectStorage(it->second.type);
}

static Json::Value apiPoolDetailToJson(const StoragePool &p) {
    Json::Value v = p.toJson();
    const int64_t free_bytes = p.total_bytes > p.used_bytes ? p.total_bytes - p.used_bytes : 0;
    const double used_percent = p.total_bytes > 0
        ? static_cast<double>(p.used_bytes) * 100.0 / static_cast<double>(p.total_bytes)
        : static_cast<double>(p.usage_pct);

    v["status"] = p.health_status.empty() ? "OK" : p.health_status;
    v["free_bytes"] = static_cast<Json::Int64>(free_bytes);
    v["used_percent"] = used_percent;
    v["secret_key"] = p.secret_key_enc.has_value() && !p.secret_key_enc.value().empty() ? "***********" : "";
    return v;
}

static Json::Value apiPolicySummaryToJson(const StoragePolicy &p, int applied_camera_count) {
    Json::Value v;
    v["id"] = p.id;
    v["name"] = p.name;
    v["description"] = p.description.value_or("");
    v["enabled"] = (p.enabled != 0);
    v["total_retention_days"] = p.total_retention_days;
    v["hot_retain_until_days"] = 0;
    v["warm_retain_until_days"] = 0;
    v["cold_retain_until_days"] = 0;
    for (const auto &tier : p.parsedTiers()) {
        if (tier.tier == "HOT") v["hot_retain_until_days"] = tier.retain_until_days;
        if (tier.tier == "WARM") v["warm_retain_until_days"] = tier.retain_until_days;
        if (tier.tier == "COLD") v["cold_retain_until_days"] = tier.retain_until_days;
    }
    v["delete_after_days"] = p.parsedDeletePolicy().delete_after_days;
    v["allow_camera_override"] = (p.allow_camera_override != 0);
    v["protect_event_video"] = (p.protect_event_video != 0);
    v["applied_camera_count"] = applied_camera_count;
    v["created_at"] = static_cast<Json::Int64>(p.created_at);
    v["updated_at"] = static_cast<Json::Int64>(p.updated_at);
    return v;
}

static Json::Value apiPolicyDetailToJson(const StoragePolicy &p, const std::unordered_map<std::string, StoragePool> &pool_map) {
    Json::Value v = apiPolicySummaryToJson(p, 0);
    Json::Value tiers(Json::arrayValue);
    for (const auto &tier : p.parsedTiers()) {
        auto tv = tier.toJson();
        auto it = pool_map.find(tier.pool_id);
        tv["pool_name"] = it != pool_map.end() ? it->second.name : "";
        tiers.append(tv);
    }
    v["tiers"] = tiers;
    v["delete_policy"] = p.parsedDeletePolicy().toJson();
    v["advanced_rules"] = p.parsedAdvancedRules().toJson();
    return v;
}

static bool apiValidateStoragePoolByType(const StoragePool &pool, std::string &err) {
    if (pool.type == "LOCAL_DISK") {
        if (!pool.mount_path.has_value() || pool.mount_path.value().empty()) {
            err = "mount_path is required for LOCAL_DISK";
            return false;
        }
    } else if (pool.type == "NAS") {
        const bool has_mount = pool.mount_path.has_value() && !pool.mount_path.value().empty();
        const bool has_network = pool.network_path.has_value() && !pool.network_path.value().empty();
        if (!has_mount && !has_network) {
            err = "mount_path or network_path is required for NAS";
            return false;
        }
    } else if (pool.type == "MINIO" || pool.type == "S3") {
        if (!pool.endpoint.has_value() || pool.endpoint.value().empty() ||
            !pool.bucket.has_value() || pool.bucket.value().empty() ||
            !pool.base_path.has_value() || pool.base_path.value().empty() ||
            !pool.access_key.has_value() || pool.access_key.value().empty() ||
            !pool.secret_key_enc.has_value() || pool.secret_key_enc.value().empty()) {
            err = "endpoint, bucket, base_path, access_key and secret_key are required for MINIO/S3";
            return false;
        }
    }
    return true;
}

static StoragePool apiPoolFromJson(const Json::Value &body) {
    StoragePool p;
    p.id = body.get("id", "").asString();
    p.name = body.get("name", "").asString();
    p.type = body.get("type", "").asString();
    p.tier = body.get("tier", "").asString();

    auto optStr = [&](const char *key) -> Optional<std::string> {
        if (body.isMember(key) && !body[key].isNull()) {
            auto value = body[key].asString();
            if (!value.empty()) return Optional<std::string>(value);
        }
        return Optional<std::string>();
    };

    p.endpoint = optStr("endpoint");
    p.bucket = optStr("bucket");
    p.base_path = optStr("base_path");
    p.access_key = optStr("access_key");
    p.mount_path = optStr("mount_path");
    p.network_path = optStr("network_path");
    if (body.isMember("secret_key") && !body["secret_key"].isNull() && body["secret_key"].asString() != "***********") {
        p.secret_key_enc = Optional<std::string>(body["secret_key"].asString());
    }
    p.enabled = body.get("enabled", true).asBool() ? 1 : 0;
    p.health_check_enabled = body.get("health_check_enabled", true).asBool() ? 1 : 0;
    p.high_watermark_percent = body.get("high_watermark_percent", 85).asInt();
    p.critical_watermark_percent = body.get("critical_watermark_percent", 90).asInt();
    return p;
}

static bool apiValidatePolicyTierMoveChain(const std::vector<PolicyTierConfig> &tiers, std::string &err) {
    const PolicyTierConfig *hot = nullptr;
    const PolicyTierConfig *warm = nullptr;
    const PolicyTierConfig *cold = nullptr;
    for (const auto &tier : tiers) {
        if (tier.tier == "HOT") hot = &tier;
        else if (tier.tier == "WARM") warm = &tier;
        else if (tier.tier == "COLD") cold = &tier;
    }

    if (warm && warm->enabled) {
        if (!hot || !hot->enabled || hot->overflow_action != "MOVE_TO_NEXT_TIER") {
            err = "WARM tier can only be enabled when HOT overflow_action is MOVE_TO_NEXT_TIER";
            return false;
        }
    }
    if (cold && cold->enabled) {
        if (!warm || !warm->enabled) {
            err = "COLD tier can only be enabled when WARM tier is enabled";
            return false;
        }
        if (warm->overflow_action != "MOVE_TO_NEXT_TIER") {
            err = "COLD tier can only be enabled when WARM overflow_action is MOVE_TO_NEXT_TIER";
            return false;
        }
    }
    return true;
}

static bool apiValidatePolicyTiers(const std::vector<PolicyTierConfig> &tiers, std::string &err) {
    if (!apiValidatePolicyTierMoveChain(tiers, err)) {
        return false;
    }

    int hot_days = -1;
    int warm_days = -1;
    for (const auto &t : tiers) {
        if (!t.enabled) continue;
        if (t.tier == "HOT") hot_days = t.retain_until_days;
        if (t.tier == "WARM") warm_days = t.retain_until_days;
        if (t.tier == "COLD") {
            if (hot_days >= 0 && warm_days >= 0 && hot_days >= warm_days) {
                err = "HOT.retain_until_days must be less than WARM.retain_until_days";
                return false;
            }
            if (warm_days >= 0 && warm_days >= t.retain_until_days) {
                err = "WARM.retain_until_days must be less than COLD.retain_until_days";
                return false;
            }
        }
        if (t.high_watermark_percent >= t.critical_watermark_percent) {
            err = "high_watermark_percent must be less than critical_watermark_percent";
            return false;
        }
    }
    return true;
}

static StoragePolicy apiPolicyFromJson(const Json::Value &body) {
    StoragePolicy p;
    p.id = body.get("id", "").asString();
    p.name = body.get("name", "").asString();
    if (body.isMember("description") && !body["description"].isNull()) {
        p.description = Optional<std::string>(body["description"].asString());
    }
    p.enabled = body.get("enabled", true).asBool() ? 1 : 0;
    p.total_retention_days = body.get("total_retention_days", 30).asInt();
    p.allow_camera_override = body.get("allow_camera_override", true).asBool() ? 1 : 0;
    p.protect_event_video = body.get("protect_event_video", false).asBool() ? 1 : 0;

    Json::FastWriter writer;
    p.tiers_json = body.isMember("tiers") && body["tiers"].isArray() ? writer.write(body["tiers"]) : "[]";
    p.delete_policy_json = body.isMember("delete_policy") && body["delete_policy"].isObject() ? writer.write(body["delete_policy"]) : "{}";
    p.advanced_rules_json = body.isMember("advanced_rules") && body["advanced_rules"].isObject() ? writer.write(body["advanced_rules"]) : "{}";
    return p;
}

static bool apiValidateStoragePolicy(const StoragePolicy &policy, std::string &err) {
    auto tiers = policy.parsedTiers();
    if (!apiValidatePolicyTiers(tiers, err)) {
        return false;
    }

    int max_retain_days = 0;
    bool hot_enabled = false;
    auto pool_map = apiLoadStoragePoolMap();
    for (const auto &tier : tiers) {
        if (!tier.enabled) continue;
        max_retain_days = std::max(max_retain_days, tier.retain_until_days);
        if (tier.tier == "HOT") hot_enabled = true;
        if (tier.pool_id.empty()) {
            err = tier.tier + ".pool_id is required";
            return false;
        }
        auto it = pool_map.find(tier.pool_id);
        if (it == pool_map.end() || it->second.enabled == 0) {
            err = "pool_id must exist and be enabled: " + tier.pool_id;
            return false;
        }
        if (tier.critical_watermark_percent > 95) {
            err = "critical_watermark_percent must be <= 95";
            return false;
        }
    }

    if (!hot_enabled) {
        err = "HOT tier must be enabled";
        return false;
    }
    if (policy.parsedDeletePolicy().delete_after_days < max_retain_days) {
        err = "delete_after_days must be greater than or equal to the largest retain_until_days";
        return false;
    }
    return true;
}

static int apiProgressPercent(int64_t processed, int64_t total, const std::string &status) {
    if (status == "DONE") return 100;
    if (total <= 0) return processed > 0 ? 100 : 0;
    return std::max(0, std::min(100, static_cast<int>((processed * 100) / total)));
}

static Json::Value apiRestoreJobToJson(const RestoreJob &j) {
    Json::Value v;
    v["job_id"] = j.job_id;
    v["status"] = j.status;
    v["progress_percent"] = apiProgressPercent(j.processed_bytes, j.total_bytes, j.status);
    v["camera_id"] = j.camera_id;
    auto device = findDeviceSource(j.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
    v["camera_name"] = device ? device->getDeviceTuple().name : "";
    v["source_tier"] = j.source_tier;
    v["target_tier"] = j.target_tier;
    v["total_bytes"] = static_cast<Json::Int64>(j.total_bytes);
    v["playback_ready"] = (j.status == "DONE");
    v["created_at"] = static_cast<Json::Int64>(j.created_at);
    return v;
}

static Json::Value apiTieringJobToJson(const TieringJob &j) {
    Json::Value v;
    v["job_id"] = j.job_id;
    v["status"] = j.status;
    v["progress_percent"] = apiProgressPercent(j.bytes_moved, j.bytes_total, j.status);
    v["camera_id"] = j.camera_id;
    v["stream_id"] = j.stream_id;
    v["range_id"] = j.range_id;
    auto device = findDeviceSource(j.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
    v["camera_name"] = device ? device->getDeviceTuple().name : "";
    v["source_tier"] = j.source_tier;
    v["target_tier"] = j.target_tier;
    v["source_pool_id"] = j.source_pool_id;
    v["target_pool_id"] = j.target_pool_id;
    v["segment_count"] = 0;
    v["processed_segment_count"] = 0;
    v["total_bytes"] = static_cast<Json::Int64>(j.bytes_total);
    v["processed_bytes"] = static_cast<Json::Int64>(j.bytes_moved);
    v["created_at"] = static_cast<Json::Int64>(j.created_at);
    v["updated_at"] = static_cast<Json::Int64>(j.updated_at);
    return v;
}

static int64_t apiLatestTieringJobTime(const std::string &camera_id) {
    TieringJobImp imp;
    auto jobs = imp.query(camera_id, "", "", "", 0, 0, 0, 1);
    if (jobs.empty()) return 0;
    return jobs.front().updated_at > 0 ? jobs.front().updated_at : jobs.front().created_at;
}

static Json::Value apiAlertToJson(const StorageAlert &a, const std::unordered_map<std::string, StoragePool> &pool_map) {
    Json::Value v;
    v["id"] = a.id;
    v["level"] = a.level;
    v["type"] = a.type;
    v["pool_id"] = a.pool_id;
    auto it = pool_map.find(a.pool_id);
    v["pool_name"] = it == pool_map.end() ? "" : it->second.name;
    v["message"] = a.message;
    v["created_at"] = static_cast<Json::Int64>(a.created_at);
    v["acknowledged"] = (a.acknowledged != 0);
    return v;
}

static Json::Value apiExpiredSegmentToJson(const SegmentTierRange &r) {
    ProtectedVideoImp protected_imp;
    Json::Value v;
    v["segment_id"] = r.range_id;
    v["range_id"] = r.range_id;
    v["camera_id"] = r.camera_id;
    v["stream_id"] = r.stream_id;
    auto device = findDeviceSource(r.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
    v["camera_name"] = device ? device->getDeviceTuple().name : "";
    v["start_time"] = static_cast<Json::Int64>(r.start_time);
    v["end_time"] = static_cast<Json::Int64>(r.end_time);
    v["tier"] = r.tier;
    v["pool_id"] = r.pool_id;
    v["segment_count"] = static_cast<Json::Int64>(r.segment_count);
    v["size_bytes"] = static_cast<Json::Int64>(r.size_bytes);
    v["expired_at"] = static_cast<Json::Int64>(r.updated_at > 0 ? r.updated_at : r.end_time);
    v["protected"] = protected_imp.overlaps(r.camera_id, r.start_time, r.end_time);
    v["evidence"] = false;
    return v;
}

static Json::Value apiProtectedVideoToJson(const ProtectedVideo &p) {
    Json::Value v;
    v["protected_id"] = p.protected_id;
    v["camera_id"] = p.camera_id;
    auto device = findDeviceSource(p.camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
    v["camera_name"] = device ? device->getDeviceTuple().name : "";
    v["start_time"] = static_cast<Json::Int64>(p.start_time);
    v["end_time"] = static_cast<Json::Int64>(p.end_time);
    v["type"] = p.type;
    v["reason"] = p.reason.value_or("");
    v["created_at"] = static_cast<Json::Int64>(p.created_at);
    return v;
}

static std::string apiWorstStorageStatus(const std::string &a, const std::string &b) {
    auto rank = [](const std::string &s) {
        if (s == "CRITICAL" || s == "OFFLINE") return 4;
        if (s == "HIGH") return 3;
        if (s == "WARNING") return 2;
        return 1;
    };
    return rank(b) > rank(a) ? b : a;
}

static Json::Value apiDashboardSummaryToJson() {
    auto pools = TierStorageManager::Instance().listPools();

    struct TierAgg {
        int64_t total = 0;
        int64_t used = 0;
        std::string status = "OK";
    };
    struct TierAggCompare {
        bool operator()(const std::string &x, const std::string &y) const { return tierTypeFromString(x) < tierTypeFromString(y); }
    };
    std::map<std::string, TierAgg, TierAggCompare> tiers;
    tiers["HOT"];
    tiers["WARM"];
    tiers["COLD"];

    int64_t total_bytes = 0;
    int64_t used_bytes = 0;
    std::string status = "OK";
    Json::Value alerts(Json::arrayValue);

    for (const auto &p : pools) {
        const std::string pool_status = p.health_status.empty() ? "OK" : p.health_status;
        auto &agg = tiers[p.tier];
        agg.total += p.total_bytes;
        agg.used += p.used_bytes;
        agg.status = apiWorstStorageStatus(agg.status, pool_status);
        total_bytes += p.total_bytes;
        used_bytes += p.used_bytes;
        status = apiWorstStorageStatus(status, pool_status);

        const int used_percent = p.total_bytes > 0
            ? static_cast<int>((p.used_bytes * 100) / p.total_bytes)
            : static_cast<int>(p.usage_pct);
        if (pool_status != "OK" || used_percent >= p.high_watermark_percent) {
            Json::Value alert;
            alert["level"] = pool_status == "OK" ? "WARNING" : pool_status;
            alert["message"] = p.name + " used percent is " + std::to_string(used_percent) + "%";
            alerts.append(alert);
        }
    }

    Json::Value tier_summary(Json::arrayValue);
    for (const auto &kv : tiers) {
        const auto &agg = kv.second;
        Json::Value t;
        t["tier"] = kv.first;
        t["used_percent"] = agg.total > 0 ? static_cast<int>((agg.used * 100) / agg.total) : 0;
        t["status"] = agg.status;
        t["total_bytes"] = static_cast<Json::Int64>(agg.total);
        t["used_bytes"] = static_cast<Json::Int64>(agg.used);
        t["estimated_remaining_days"] = 0.0;
        t["write_mbps"] = 0;
        tier_summary.append(t);
    }

    TieringJobImp tiering_imp;
    RestoreJobImp restore_imp;
    Json::Value data;
    data["total_bytes"] = static_cast<Json::Int64>(total_bytes);
    data["used_bytes"] = static_cast<Json::Int64>(used_bytes);
    data["free_bytes"] = static_cast<Json::Int64>(total_bytes > used_bytes ? total_bytes - used_bytes : 0);
    data["used_percent"] = total_bytes > 0 ? static_cast<int>((used_bytes * 100) / total_bytes) : 0;
    data["status"] = status;
    data["tier_summary"] = tier_summary;
    data["active_tiering_jobs"] = tiering_imp.countQuery("", "PENDING") + tiering_imp.countQuery("", "RUNNING");
    data["failed_tiering_jobs"] = tiering_imp.countQuery("", "FAILED");
    data["active_restore_jobs"] = restore_imp.countActive();
    data["alerts"] = alerts;
    return data;
}

/**
 * Install api interface
 * All apis support GET and POST methods
 * POST method parameters support application/json and application/x-www-form-urlencoded methods
 */
void installWebApi() {
    addHttpListener();
    GET_CONFIG(string, api_secret, API::kSecret);

    // Get thread load
    // Test url http://127.0.0.1/index/api/getThreadsLoad
    api_regist("/index/api/getThreadsLoad", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        getThreadsLoad(EventPollerPool::Instance(), API_ARGS_VALUE, invoker);
    });

    // Get background worker thread load
    // Test url http://127.0.0.1/index/api/getWorkThreadsLoad
    api_regist("/index/api/getWorkThreadsLoad", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        getThreadsLoad(WorkThreadPool::Instance(), API_ARGS_VALUE, invoker);
    });

    // Get server configuration
    // Test url http://127.0.0.1/index/api/getServerConfig
    api_regist("/index/api/getServerConfig", [](API_ARGS_MAP) {
        CHECK_SECRET();
        Value obj;
        for (auto &pr : mINI::Instance()) {
            obj[pr.first] = (string &)pr.second;
        }
        val["data"].append(obj);
    });

    // Set server configuration
    // Test url (e.g. disable http api debugging) http://127.0.0.1/index/api/setServerConfig?api.apiDebug=0
    // You can also pass parameters through http post method, you can pass parameters through application/x-www-form-urlencoded or application/json methods
    api_regist("/index/api/setServerConfig", [](API_ARGS_MAP) {
        CHECK_SECRET();
        auto &ini = mINI::Instance();
        int changed = API::Success;
        for (auto &pr : allArgs.args) {
            if (ini.find(pr.first) == ini.end()) {
#if 1
                // This key does not exist
                continue;
#else
                // Add configuration options to dynamically add multiple ffmpeg cmd templates
                ini[pr.first] = pr.second;
                // Prevent changed changes
                continue;
#endif
            }
            if (pr.first == FFmpeg::kBin) {
                WarnL << "Configuration named " << FFmpeg::kBin << " is not allowed to be set by setServerConfig api.";
                continue;
            }
            if (ini[pr.first] == pr.second) {
                continue;
            }
            ini[pr.first] = pr.second;
            // Replacement successful
            ++changed;
        }
        if (changed > 0) {
            NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
            ini.dumpFile(g_ini_file);
        }
        val["changed"] = changed;
    });

    static auto s_get_api_list = [](API_ARGS_MAP) {
        CHECK_SECRET();
        for (auto &pr : s_map_api) {
            val["data"].append(pr.first);
        }
    };

    // Get server api list
    // Test url http://127.0.0.1/index/api/getApiList
    api_regist("/index/api/getApiList",[](API_ARGS_MAP){
        s_get_api_list(API_ARGS_VALUE);
    });

    // Get server api list
    // Test url http://127.0.0.1/index/
    api_regist("/index/",[](API_ARGS_MAP){
        s_get_api_list(API_ARGS_VALUE);
    });

#if !defined(_WIN32)
    // Restart server, only Daemon mode can restart, otherwise it will be closed directly!
    // Test url http://127.0.0.1/index/api/restartServer
    api_regist("/index/api/restartServer", [](API_ARGS_MAP) {
        CHECK_SECRET();
        EventPollerPool::Instance().getPoller()->doDelayTask(1000, []() {
            // Try to exit normally
            ::kill(getpid(), SIGINT);

            // Force exit after 3 seconds
            EventPollerPool::Instance().getPoller()->doDelayTask(3000, []() {
                exit(0);
                return 0;
            });

            return 0;
        });
        val["msg"] = "MediaServer will reboot in on 1 second";
    });
#else
    // Add restart code for Windows
    api_regist("/index/api/restartServer", [](API_ARGS_MAP) {
        CHECK_SECRET();
        // Create a restart batch script file
        FILE *pf;
        errno_t err = ::_wfopen_s(&pf, L"RestartServer.cmd", L"w"); //"w" If the file exists, its contents will be overwritten
        if (err == 0) {
            char szExeName[1024];
            char drive[_MAX_DRIVE] = { 0 };
            char dir[_MAX_DIR] = { 0 };
            char fname[_MAX_FNAME] = { 0 };
            char ext[_MAX_EXT] = { 0 };
            char exeName[_MAX_FNAME] = { 0 };
            GetModuleFileNameA(NULL, szExeName, 1024); // Get the full path of the process
            _splitpath(szExeName, drive, dir, fname, ext);
            strcpy(exeName, fname);
            strcat(exeName, ext);
            fprintf(pf, "@echo off\ntaskkill /f /im %s\nstart \"\" \"%s\"\ndel %%0", exeName, szExeName);
            fclose(pf);
            // Execute the created batch script after 1 second
            EventPollerPool::Instance().getPoller()->doDelayTask(1000, []() {
                STARTUPINFO si;
                PROCESS_INFORMATION pi;
                ZeroMemory(&si, sizeof si);
                ZeroMemory(&pi, sizeof pi);
                si.cb = sizeof si;
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                TCHAR winSysDir[1024];
                ZeroMemory(winSysDir, sizeof winSysDir);
                GetSystemDirectory(winSysDir, 1024);
                TCHAR appName[1024];
                ZeroMemory(appName, sizeof appName);

                _stprintf(appName, "%s\\cmd.exe", winSysDir);
                BOOL bRet = CreateProcess(appName, " /c RestartServer.cmd", NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

                if (bRet == FALSE) {
                    int err = GetLastError();
                    cout << endl << "Unable to perform restart operation, error code：" << err << endl;
                }
                WaitForSingleObject(pi.hProcess, INFINITE);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return 0;
            });
            val["msg"] = "The server will automatically restart after one second";
        } else {
            val["msg"] = "Failed to create a restart script file";
            val["code"] = API::OtherFailed;
        }
    });
#endif // #if !defined(_WIN32)

    // Get stream list, optional filtering parameters
    // Test url0 (get all streams) http://127.0.0.1/index/api/getMediaList
    // Test url1 (get streams with virtual host "__defaultVost__") http://127.0.0.1/index/api/getMediaList?vhost=__defaultVost__
    // Test url2 (get rtsp type streams) http://127.0.0.1/index/api/getMediaList?schema=rtsp
    api_regist("/index/api/getMediaList",[](API_ARGS_MAP_ASYNC){
        CHECK_SECRET();
        // Get all MediaSource lists
        std::list<MediaSource::Ptr> lst;
        MediaSource::for_each_media([&](const MediaSource::Ptr &media) {
            lst.emplace_back(media);
        }, allArgs["schema"], allArgs["vhost"], allArgs["app"], allArgs["stream"]);

        if (lst.size() == 1) {
           //If searching for a single stream, execute it in its own thread to obtain the packet loss rate parameter
            auto front = std::move(lst.front());
            front->getOwnerPoller()->async([=]() mutable {
                val["data"].append(makeMediaSourceJson(*front));
                invoker(200, headerOut, val.toStyledString());
            });
        } else {
            for (auto &media : lst) {
                val["data"].append(makeMediaSourceJson(*media));
            }
            invoker(200, headerOut, val.toStyledString());
        }
    });

    // Test url http://127.0.0.1/index/api/isMediaOnline?schema=rtsp&vhost=__defaultVhost__&app=live&stream=obs
    api_regist("/index/api/isMediaOnline", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("schema","vhost","app","stream");
        val["online"] = (bool) (MediaSource::find(allArgs["schema"],allArgs["vhost"],allArgs["app"],allArgs["stream"]));
    });

    // Get media stream player list
    // Test url http://127.0.0.1/index/api/getMediaPlayerList?schema=rtsp&vhost=__defaultVhost__&app=live&stream=obs
    api_regist("/index/api/getMediaPlayerList", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream");
        auto src = MediaSource::find(allArgs["schema"], allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }
        src->getPlayerList(
            [=](const std::list<toolkit::Any> &info_list) mutable {
                val["code"] = API::Success;
                auto &data = val["data"];
                data = Value(arrayValue);
                for (auto &info : info_list) {
                    auto &obj = info.get<Value>();
                    data.append(std::move(obj));
                }
                invoker(200, headerOut, val.toStyledString());
            },
            [](toolkit::Any &&info) -> toolkit::Any {
                auto obj = std::make_shared<Value>();
                auto &session = info.get<Session>();
                fillSockInfo(*obj, &session);
                (*obj)["typeid"] = toolkit::demangle(typeid(session).name());
                toolkit::Any ret;
                ret.set(obj);
                return ret;
            });
    });

    api_regist("/index/api/broadcastMessage", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream", "msg");
        auto src = MediaSource::find(allArgs["schema"], allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }
        Any any;
        Buffer::Ptr buffer = std::make_shared<BufferLikeString>(allArgs["msg"]);
        any.set(std::move(buffer));
        src->broadcastMessage(any);
    });

    // Test url http://127.0.0.1/index/api/getMediaInfo?schema=rtsp&vhost=__defaultVhost__&app=live&stream=obs
    api_regist("/index/api/getMediaInfo", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream");
        auto src = MediaSource::find(allArgs["schema"], allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }
        src->getOwnerPoller()->async([=]() mutable {
            auto val = makeMediaSourceJson(*src);
            val["code"] = API::Success;
            invoker(200, headerOut, val.toStyledString());
        });
    });

    // Actively close the stream, including closing the pull stream and push stream
    // Test url http://127.0.0.1/index/api/close_stream?schema=rtsp&vhost=__defaultVhost__&app=live&stream=obs&force=1
    api_regist("/index/api/close_stream", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream");
        // Kick out the pusher
        auto src = MediaSource::find(allArgs["schema"],
                                     allArgs["vhost"],
                                     allArgs["app"],
                                     allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }

        bool force = allArgs["force"].as<bool>();
        src->getOwnerPoller()->async([=]() mutable {
            bool flag = src->close(force);
            val["result"] = flag ? 0 : -1;
            val["msg"] = flag ? "success" : "close failed";
            val["code"] = flag ? API::Success : API::OtherFailed;
            invoker(200, headerOut, val.toStyledString());
        });
    });

    // Batch actively close the stream, including closing the pull stream and push stream
    // Test url http://127.0.0.1/index/api/close_streams?schema=rtsp&vhost=__defaultVhost__&app=live&stream=obs&force=1
    api_regist("/index/api/close_streams", [](API_ARGS_MAP) {
        CHECK_SECRET();
        // Filter hit count
        int count_hit = 0;
        int count_closed = 0;
        list<MediaSource::Ptr> media_list;
        MediaSource::for_each_media([&](const MediaSource::Ptr &media) {
            ++count_hit;
            media_list.emplace_back(media);
        }, allArgs["schema"], allArgs["vhost"], allArgs["app"], allArgs["stream"]);

        bool force = allArgs["force"].as<bool>();
        for (auto &media : media_list) {
            media->getOwnerPoller()->async([media, force]() { media->close(force); });
            ++count_closed;
        }
        val["count_hit"] = count_hit;
        val["count_closed"] = count_closed;
    });

    // Get all Session list information
    // You can filter by local port and remote ip
    // Test url (filter tcp session under a certain port) http://127.0.0.1/index/api/getAllSession?local_port=1935
    api_regist("/index/api/getAllSession", [](API_ARGS_MAP) {
        CHECK_SECRET();
        Value jsession;
        uint16_t local_port = allArgs["local_port"].as<uint16_t>();
        string peer_ip = allArgs["peer_ip"];

        SessionMap::Instance().for_each_session([&](const string &id, const Session::Ptr &session) {
            if (local_port != 0 && local_port != session->get_local_port()) {
                return;
            }
            if (!peer_ip.empty() && peer_ip != session->get_peer_ip()) {
                return;
            }
            fillSockInfo(jsession, session.get());
            jsession["id"] = id;
            jsession["typeid"] = toolkit::demangle(typeid(*session).name());
            val["data"].append(jsession);
        });
    });

    // Disconnect the tcp connection, for example, you can disconnect the rtsp, rtmp player, etc.
    // Test url http://127.0.0.1/index/api/kick_session?id=123456
    api_regist("/index/api/kick_session", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("id");
        // Kick out the tcp session
        auto session = SessionMap::Instance().get(allArgs["id"]);
        if (!session) {
            throw ApiRetException("can not find the target", API::OtherFailed);
        }
        session->safeShutdown();
    });

    // Batch disconnect tcp connections, for example, you can disconnect rtsp, rtmp players, etc.
    // Test url http://127.0.0.1/index/api/kick_sessions?local_port=1935
    api_regist("/index/api/kick_sessions", [](API_ARGS_MAP) {
        CHECK_SECRET();
        uint16_t local_port = allArgs["local_port"].as<uint16_t>();
        string peer_ip = allArgs["peer_ip"];
        size_t count_hit = 0;

        list<Session::Ptr> session_list;
        SessionMap::Instance().for_each_session([&](const string &id, const Session::Ptr &session) {
            if (local_port != 0 && local_port != session->get_local_port()) {
                return;
            }
            if (!peer_ip.empty() && peer_ip != session->get_peer_ip()) {
                return;
            }
            if (session->getIdentifier() == sender.getIdentifier()) {
                // Ignore this http link
                return;
            }
            session_list.emplace_back(session);
            ++count_hit;
        });

        for (auto &session : session_list) {
            session->safeShutdown();
        }
        val["count_hit"] = (Json::UInt64)count_hit;
    });

    // Dynamically add rtsp/rtmp push stream proxy
    // Test url http://127.0.0.1/index/api/addStreamPusherProxy?schema=rtmp&vhost=__defaultVhost__&app=proxy&stream=0&dst_url=rtmp://127.0.0.1/live/obs
    api_regist("/index/api/addStreamPusherProxy", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream", "dst_url");

        mINI args;
        for (auto &pr : allArgs.args) {
            args.emplace(pr.first, pr.second);
        }

        auto dst_url = allArgs["dst_url"];
        auto retry_count = allArgs["retry_count"].empty() ? -1 : allArgs["retry_count"].as<int>();
        EventPollerPool::Instance().getPoller(false)->async([=]() mutable {
            addStreamPusherProxy(allArgs["schema"],
                                 allArgs["vhost"],
                                 allArgs["app"],
                                 allArgs["stream"],
                                 allArgs["dst_url"],
                                 retry_count,
                                 allArgs["rtp_type"],
                                 allArgs["timeout_sec"],
                                 args,
                                 [invoker, val, headerOut, dst_url](const SockException &ex, const string &key) mutable {
                                     if (ex) {
                                         val["code"] = API::OtherFailed;
                                         val["msg"] = ex.what();
                                     } else {
                                         val["data"]["key"] = key;
                                         InfoL << "Publish success, please play with player:" << dst_url;
                                     }
                                     invoker(200, headerOut, val.toStyledString());
                                 });
        });
    });

    // Close the push stream proxy
    // Test url http://127.0.0.1/index/api/delStreamPusherProxy?key=__defaultVhost__/proxy/0
    api_regist("/index/api/delStreamPusherProxy", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("key");
        val["data"]["flag"] = s_pusher_proxy.erase(allArgs["key"]) == 1;
    });
    api_regist("/index/api/listStreamPusherProxy", [](API_ARGS_MAP) {
        CHECK_SECRET();
        s_pusher_proxy.for_each([&val](const std::string &key, const PusherProxy::Ptr &p) {
            Json::Value item = ToJson(p);
            item["key"] = key;
            val["data"].append(item);
        });
    });
    api_regist("/index/api/listStreamProxy", [](API_ARGS_MAP) {
        CHECK_SECRET();
        s_player_proxy.for_each([&val](const std::string &key, const PlayerProxy::Ptr &p) {
            Json::Value item = ToJson(p);
            item["key"] = key;
            val["data"].append(item);
        });
    });
    // Dynamically add rtsp/rtmp pull stream proxy
    // Test url http://127.0.0.1/index/api/addStreamProxy?vhost=__defaultVhost__&app=proxy&enable_rtsp=1&enable_rtmp=1&stream=0&url=rtmp://127.0.0.1/live/obs
    api_regist("/index/api/addStreamProxy", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream", "url");

        mINI args;
        for (auto &pr : allArgs.args) {
            args.emplace(pr.first, pr.second);
        }

        ProtocolOption option(allArgs);
        auto retry_count = allArgs["retry_count"].empty() ? -1 : allArgs["retry_count"].as<int>();

        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        auto tuple = MediaTuple { vhost, allArgs["app"], allArgs["stream"], "" };
        EventPollerPool::Instance().getPoller(false)->async([=]() mutable {
            addStreamProxy(tuple,
                           allArgs["url"],
                           retry_count,
                           option,
                           allArgs["rtp_type"],
                           allArgs["timeout_sec"],
                           args,
                           [invoker,val,headerOut](const SockException &ex,const string &key) mutable {
                               if (ex) {
                                   val["code"] = API::OtherFailed;
                                   val["msg"] = ex.what();
                               } else {
                                   val["data"]["key"] = key;
                               }
                               invoker(200, headerOut, val.toStyledString());
                           });
        });
    });

    // Close the pull stream proxy
    // Test url http://127.0.0.1/index/api/delStreamProxy?key=__defaultVhost__/proxy/0
    api_regist("/index/api/delStreamProxy", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("key");
        val["data"]["flag"] = s_player_proxy.erase(allArgs["key"]) == 1;
    });

    static auto addFFmpegSource = [](const string &ffmpeg_cmd_key,
                                     const string &src_url,
                                     const string &dst_url,
                                     int timeout_ms,
                                     bool enable_hls,
                                     bool enable_mp4,
                                     const function<void(const SockException &ex, const string &key)> &cb) {
        auto key = MD5(dst_url).hexdigest();
        if (s_ffmpeg_src.find(key)) {
            // Already pulling
            cb(SockException(Err_success), key);
            return;
        }

        auto ffmpeg = s_ffmpeg_src.make(key);

        ffmpeg->setOnClose([key]() {
            s_ffmpeg_src.erase(key);
        });
        ffmpeg->setupRecordFlag(enable_hls, enable_mp4);
        ffmpeg->play(ffmpeg_cmd_key, src_url, dst_url, timeout_ms, [cb, key](const SockException &ex) {
            if (ex) {
                s_ffmpeg_src.erase(key);
            }
            cb(ex, key);
        });
    };

    // Dynamically add rtsp/rtmp pull stream proxy
    // Test url http://127.0.0.1/index/api/addFFmpegSource?src_url=http://live.hkstv.hk.lxdns.com/live/hks2/playlist.m3u8&dst_url=rtmp://127.0.0.1/live/hks2&timeout_ms=10000
    api_regist("/index/api/addFFmpegSource",[](API_ARGS_MAP_ASYNC){
        CHECK_SECRET();
        CHECK_ARGS("src_url", "dst_url", "timeout_ms");
        auto src_url = allArgs["src_url"];
        auto dst_url = allArgs["dst_url"];
        int timeout_ms = allArgs["timeout_ms"];
        auto enable_hls = allArgs["enable_hls"].as<int>();
        auto enable_mp4 = allArgs["enable_mp4"].as<int>();

        addFFmpegSource(allArgs["ffmpeg_cmd_key"], src_url, dst_url, timeout_ms, enable_hls, enable_mp4,
                        [invoker, val, headerOut](const SockException &ex, const string &key) mutable{
            if (ex) {
                val["code"] = API::OtherFailed;
                val["msg"] = ex.what();
            } else {
                val["data"]["key"] = key;
            }
            invoker(200, headerOut, val.toStyledString());
        });
    });

    // Close the pull stream proxy
    // Test url http://127.0.0.1/index/api/delFFmepgSource?key=key
    api_regist("/index/api/delFFmpegSource", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("key");
        val["data"]["flag"] = s_ffmpeg_src.erase(allArgs["key"]) == 1;
    });
    api_regist("/index/api/listFFmpegSource", [](API_ARGS_MAP) {
        CHECK_SECRET();
        s_ffmpeg_src.for_each([&val](const std::string &key, const FFmpegSource::Ptr &src) {
            Json::Value item;
            item["src_url"] = src->getSrcUrl();
            item["dst_url"] = src->getDstUrl();
            item["cmd"] = src->getCmd();
            item["ffmpeg_cmd_key"] = src->getCmdKey();
            item["key"] = key;
            val["data"].append(item);
        });
    });
    // Add a new http api to download executable files
    // Test url http://127.0.0.1/index/api/downloadBin
    api_regist("/index/api/downloadBin", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        invoker.responseFile(allArgs.parser.getHeader(), StrCaseMap(), exePath());
    });

#if defined(ENABLE_RTPPROXY)
    api_regist("/index/api/getRtpInfo", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("stream_id");
        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        std::string app = kRtpAppName;
        if (!allArgs["app"].empty()) {
            app = allArgs["app"];
        }
        auto src = MediaSource::find(vhost, app, allArgs["stream_id"]);
        auto process = src ? src->getRtpProcess() : nullptr;
        if (!process) {
            val["exist"] = false;
            return;
        }
        val["exist"] = true;
        fillSockInfo(val, process.get());
    });

    api_regist("/index/api/openRtpServer", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("port", "stream_id");
        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        std::string app = kRtpAppName;
        if (!allArgs["app"].empty()) {
            app = allArgs["app"];
        }
        auto stream_id = allArgs["stream_id"];
        auto tuple = MediaTuple { vhost, app, stream_id, "" };
        auto tcp_mode = allArgs["tcp_mode"].as<int>();
        if (allArgs["enable_tcp"].as<int>() && !tcp_mode) {
            // Compatible with old version requests, the new version removes the enable_tcp parameter and adds the tcp_mode parameter
            tcp_mode = 1;
        }
        auto only_track = allArgs["only_track"].as<int>();
        if (allArgs["only_audio"].as<bool>()) {
            // Compatible with old version requests, the new version removes the only_audio parameter and adds the only_track parameter
            only_track = 1;
        }
        GET_CONFIG(std::string, local_ip, General::kListenIP)
        if (!allArgs["local_ip"].empty()) {
            local_ip = allArgs["local_ip"];
        }
        auto port = openRtpServer(allArgs["port"], tuple, tcp_mode, local_ip, allArgs["re_use_port"].as<bool>(),
                                  allArgs["ssrc"].as<uint32_t>(), only_track);
        if (port == 0) {
            throw InvalidArgsException("This stream already exists");
        }
        // Reply json
        val["port"] = port;
    });

    api_regist("/index/api/openRtpServerMultiplex", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("port", "stream_id");
        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        std::string app = kRtpAppName;
        if (!allArgs["app"].empty()) {
            app = allArgs["app"];
        }
        auto stream_id = allArgs["stream_id"];
        auto tuple = MediaTuple { vhost, app, stream_id, "" };
        auto tcp_mode = allArgs["tcp_mode"].as<int>();
        if (allArgs["enable_tcp"].as<int>() && !tcp_mode) {
            // Compatible with old version requests, the new version removes the enable_tcp parameter and adds the tcp_mode parameter
            tcp_mode = 1;
        }
        auto only_track = allArgs["only_track"].as<int>();
        if (allArgs["only_audio"].as<bool>()) {
            // Compatible with old version requests, the new version removes the only_audio parameter and adds the only_track parameter
            only_track = 1;
        }
        std::string local_ip = "::";
        if (!allArgs["local_ip"].empty()) {
            local_ip = allArgs["local_ip"];
        }

        auto port = openRtpServer(allArgs["port"], tuple, tcp_mode, local_ip, true, 0, only_track, true);
        if (port == 0) {
            throw InvalidArgsException("This stream already exists");
        }
        // Reply json
        val["port"] = port;
    });

    api_regist("/index/api/connectRtpServer", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("stream_id", "dst_url", "dst_port");
        auto cb = [val, headerOut, invoker](const SockException &ex) mutable {
            if (ex) {
                val["code"] = API::OtherFailed;
                val["msg"] = ex.what();
            }
            invoker(200, headerOut, val.toStyledString());
        };

        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        std::string app = kRtpAppName;
        if (!allArgs["app"].empty()) {
            app = allArgs["app"];
        }
        auto stream_id = allArgs["stream_id"];
        auto tuple = MediaTuple { vhost, app, stream_id, "" };
        auto server = s_rtp_server.find(tuple.shortUrl());
        if (!server) {
            cb(SockException(Err_other, "can not find the stream"));
            return;
        }
        server->connectToServer(allArgs["dst_url"], allArgs["dst_port"], cb);
    });

    api_regist("/index/api/closeRtpServer", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("stream_id");

        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        std::string app = kRtpAppName;
        if (!allArgs["app"].empty()) {
            app = allArgs["app"];
        }
        auto stream_id = allArgs["stream_id"];
        auto tuple = MediaTuple { vhost, app, stream_id, "" };
        if (s_rtp_server.erase(tuple.shortUrl()) == 0) {
            val["hit"] = 0;
            return;
        }
        val["hit"] = 1;
    });

    api_regist("/index/api/updateRtpServerSSRC", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("stream_id", "ssrc");

        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        std::string app = kRtpAppName;
        if (!allArgs["app"].empty()) {
            app = allArgs["app"];
        }
        auto stream_id = allArgs["stream_id"];
        auto tuple = MediaTuple { vhost, app, stream_id, "" };
        auto server = s_rtp_server.find(tuple.shortUrl());
        if (!server) {
            throw ApiRetException("RtpServer not found by stream_id", API::NotFound);
        }
        server->updateSSRC(allArgs["ssrc"]);
    });

    api_regist("/index/api/listRtpServer", [](API_ARGS_MAP) {
        CHECK_SECRET();

        s_rtp_server.for_each([&val](const std::string &key, const RtpServer::Ptr &rtps) {
            auto vec = split(key, "/");
            Value obj;
            obj["vhost"] = vec[0];
            obj["app"] = vec[1];
            obj["stream_id"] = vec[2];
            obj["port"] = rtps->getPort();
            obj["ssrc"] = rtps->getSSRC();
            obj["tcp_mode"] = rtps->getTcpMode();
            obj["only_track"] = rtps->getOnlyTrack();
            val["data"].append(obj);
        });
    });

    static auto start_send_rtp = [](bool passive, API_ARGS_MAP_ASYNC) {
        auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"], allArgs["from_mp4"].as<int>());
        if (!src) {
            throw ApiRetException("can not find the source stream", API::NotFound);
        }
        auto type = allArgs["type"].empty() ? (int)MediaSourceEvent::SendRtpArgs::kRtpPS : allArgs["type"].as<int>();
        if (!allArgs["use_ps"].empty()) {
            // Compatible with the previous use_ps parameter
            type = allArgs["use_ps"].as<int>();
        }
        MediaSourceEvent::SendRtpArgs args;
        if (passive) {
            args.con_type = allArgs["is_udp"].as<bool>() ? mediakit::MediaSourceEvent::SendRtpArgs::kUdpPassive : mediakit::MediaSourceEvent::SendRtpArgs::kTcpPassive;
        } else {
            args.con_type = allArgs["is_udp"].as<bool>() ? mediakit::MediaSourceEvent::SendRtpArgs::kUdpActive : mediakit::MediaSourceEvent::SendRtpArgs::kTcpActive;
        }
        args.dst_url = allArgs["dst_url"];
        args.dst_port = allArgs["dst_port"];
        args.ssrc_multi_send = allArgs["ssrc_multi_send"].empty() ? false : allArgs["ssrc_multi_send"].as<bool>();
        args.ssrc = allArgs["ssrc"];
        args.src_port = allArgs["src_port"];
        args.pt = allArgs["pt"].empty() ? 96 : allArgs["pt"].as<int>();
        args.data_type = (MediaSourceEvent::SendRtpArgs::DataType)type;
        args.only_audio = allArgs["only_audio"].as<bool>();
        args.udp_rtcp_timeout = allArgs["udp_rtcp_timeout"];
        args.recv_stream_id = allArgs["recv_stream_id"];
        args.close_delay_ms = allArgs["close_delay_ms"];
        // Record the app and vhost of the sending stream
        args.recv_stream_app = allArgs["app"];
        args.recv_stream_vhost = allArgs["vhost"];
        args.enable_origin_recv_limit = allArgs["enable_origin_recv_limit"];
        src->getOwnerPoller()->async([=]() mutable {
            try {
                src->startSendRtp(args, [val, headerOut, invoker](uint16_t local_port, const SockException &ex) mutable {
                    if (ex) {
                        val["code"] = API::OtherFailed;
                        val["msg"] = ex.what();
                    }
                    val["local_port"] = local_port;
                    invoker(200, headerOut, val.toStyledString());
                });
            } catch (std::exception &ex) {
                val["code"] = API::Exception;
                val["msg"] = ex.what();
                invoker(200, headerOut, val.toStyledString());
            }
        });
    };

    api_regist("/index/api/startSendRtp", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream", "ssrc", "dst_url", "dst_port", "is_udp");
        start_send_rtp(false, API_ARGS_VALUE, invoker);
    });

    api_regist("/index/api/startSendRtpPassive", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream", "ssrc");
        start_send_rtp(true, API_ARGS_VALUE, invoker);
    });

    api_regist("/index/api/startSendRtpTalk", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream", "ssrc", "recv_stream_id");
        auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"], allArgs["from_mp4"].as<int>());
        if (!src) {
            throw ApiRetException("can not find the source stream", API::NotFound);
        }
        MediaSourceEvent::SendRtpArgs args;
        args.con_type = mediakit::MediaSourceEvent::SendRtpArgs::kVoiceTalk;
        args.ssrc = allArgs["ssrc"];
        args.pt = allArgs["pt"].empty() ? 96 : allArgs["pt"].as<int>();
        args.data_type = allArgs["type"].empty() ? MediaSourceEvent::SendRtpArgs::kRtpPS : (MediaSourceEvent::SendRtpArgs::DataType)(allArgs["type"].as<int>());
        args.only_audio = allArgs["only_audio"].as<bool>();
        args.recv_stream_id = allArgs["recv_stream_id"];
        args.recv_stream_app = allArgs["app"];
        args.recv_stream_vhost = allArgs["vhost"];
        args.enable_origin_recv_limit = allArgs["enable_origin_recv_limit"];

        src->getOwnerPoller()->async([=]() mutable {
            try {
                src->startSendRtp(args, [val, headerOut, invoker](uint16_t local_port, const SockException &ex) mutable {
                    if (ex) {
                        val["code"] = API::OtherFailed;
                        val["msg"] = ex.what();
                    }
                    val["local_port"] = local_port;
                    invoker(200, headerOut, val.toStyledString());
                });
            } catch (std::exception &ex) {
                val["code"] = API::Exception;
                val["msg"] = ex.what();
                invoker(200, headerOut, val.toStyledString());
            }
        });
    });

    api_regist("/index/api/listRtpSender", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream");

        auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the source stream", API::NotFound);
        }

        auto muxer = src->getMuxer();
        CHECK(muxer, "get muxer from media source failed");

        src->getOwnerPoller()->async([=]() mutable {
            muxer->forEachRtpSender([&](const std::string &ssrc, const RtpSender &sender) mutable {
                val["data"].append(ssrc);
                val["bytesSpeed"] = (Json::UInt64)sender.getSendSpeed();
                val["totalBytes"] = (Json::UInt64)sender.getSendTotalBytes();
            });
            invoker(200, headerOut, val.toStyledString());
        });
    });

    api_regist("/index/api/stopSendRtp", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream");

        auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }

        src->getOwnerPoller()->async([=]() mutable {
            // If ssrc is empty, close all
            if (!src->stopSendRtp(allArgs["ssrc"])) {
                val["code"] = API::OtherFailed;
                val["msg"] = "stopSendRtp failed";
                invoker(200, headerOut, val.toStyledString());
                return;
            }
            invoker(200, headerOut, val.toStyledString());
        });
    });

    api_regist("/index/api/pauseRtpCheck", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("stream_id");
        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        std::string app = kRtpAppName;
        if (!allArgs["app"].empty()) {
            app = allArgs["app"];
        }
        // Only pause the stream check, the media server acts as a stream load balancing service, receiving the stream and forwarding it, RTSP/RTMP has its own pause protocol
        auto src = MediaSource::find(vhost, app, allArgs["stream_id"]);
        auto process = src ? src->getRtpProcess() : nullptr;
        if (process) {
            process->pauseRtpTimeout(true, allArgs["pause_seconds"]);
        } else {
            val["code"] = API::NotFound;
        }
    });

    api_regist("/index/api/resumeRtpCheck", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("stream_id");
        std::string vhost = DEFAULT_VHOST;
        if (!allArgs["vhost"].empty()) {
            vhost = allArgs["vhost"];
        }
        std::string app = kRtpAppName;
        if (!allArgs["app"].empty()) {
            app = allArgs["app"];
        }
        auto src = MediaSource::find(vhost, app, allArgs["stream_id"]);
        auto process = src ? src->getRtpProcess() : nullptr;
        if (process) {
            process->pauseRtpTimeout(false);
        } else {
            val["code"] = API::NotFound;
        }
    });

#endif // ENABLE_RTPPROXY

    // Start recording hls or MP4
    api_regist("/index/api/startRecord", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("type", "vhost", "app", "stream");

        auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }

        src->getOwnerPoller()->async([=]() mutable {
            auto result = src->setupRecord((Recorder::type)allArgs["type"].as<int>(), true, allArgs["customized_path"], allArgs["max_second"].as<size_t>());
            val["result"] = result;
            val["code"] = result ? API::Success : API::OtherFailed;
            val["msg"] = result ? "success" : "start record failed";
            invoker(200, headerOut, val.toStyledString());
        });
    });

    api_regist("/index/api/startRecordTask",[](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream", "path", "back_ms", "forward_ms");

        auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }

        src->getOwnerPoller()->async([=]() mutable {
            std::string err;
            std::string path;
            try {
                path = src->getMuxer()->startRecord(allArgs["path"], allArgs["back_ms"], allArgs["forward_ms"]);
            } catch (std::exception &ex) {
                err = ex.what();
            }
            val["code"] = err.empty() ? API::Success : API::OtherFailed;
            val["data"]["path"] = path;
            val["msg"] = err;
            invoker(200, headerOut, val.toStyledString());
        });
    });

    // Set the playback speed of the recording stream
    api_regist("/index/api/setRecordSpeed", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream", "speed");
        auto src = MediaSource::find(allArgs["schema"],
                                     allArgs["vhost"],
                                     allArgs["app"],
                                     allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }

        auto speed = allArgs["speed"].as<float>();
        src->getOwnerPoller()->async([=]() mutable {
            bool flag = src->speed(speed);
            val["result"] = flag ? 0 : -1;
            val["msg"] = flag ? "success" : "set failed";
            val["code"] = flag ? API::Success : API::OtherFailed;
            invoker(200, headerOut, val.toStyledString());
        });
    });

    api_regist("/index/api/seekRecordStamp", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream", "stamp");
        auto src = MediaSource::find(allArgs["schema"],
                                     allArgs["vhost"],
                                     allArgs["app"],
                                     allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }

        auto stamp = allArgs["stamp"].as<size_t>();
        src->getOwnerPoller()->async([=]() mutable {
            bool flag = src->seekTo(stamp);
            val["result"] = flag ? 0 : -1;
            val["msg"] = flag ? "success" : "seek failed";
            val["code"] = flag ? API::Success : API::OtherFailed;
            invoker(200, headerOut, val.toStyledString());
        });
    });

    // Stop recording hls or MP4
    api_regist("/index/api/stopRecord", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("type", "vhost", "app", "stream");

        auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }

        auto type = (Recorder::type)allArgs["type"].as<int>();
        src->getOwnerPoller()->async([=]() mutable {
            auto result = src->setupRecord(type, false, "", 0);
            val["result"] = result;
            val["code"] = result ? API::Success : API::OtherFailed;
            val["msg"] = result ? "success" : "stop record failed";
            invoker(200, headerOut, val.toStyledString());
        });
    });

    // Get the recording status of hls or MP4
    api_regist("/index/api/isRecording", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("type", "vhost", "app", "stream");

        auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"]);
        if (!src) {
            throw ApiRetException("can not find the stream", API::NotFound);
        }

        auto type = (Recorder::type)allArgs["type"].as<int>();
        src->getOwnerPoller()->async([=]() mutable {
            val["status"] = src->isRecording(type);
            invoker(200, headerOut, val.toStyledString());
        });
    });

    api_regist("/index/api/getProxyPusherInfo", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("key");
        auto pusher = s_pusher_proxy.find(allArgs["key"]);
        if (!pusher) {
            throw ApiRetException("can not find pusher", API::NotFound);
        }

        val["data"] = ToJson(pusher);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/index/api/getProxyInfo", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("key");
        auto proxy = s_player_proxy.find(allArgs["key"]);
        if (!proxy) {
            throw ApiRetException("can not find the proxy", API::NotFound);
        }

        val["data"] = ToJson(proxy);
        invoker(200, headerOut, val.toStyledString());
    });

    // Delete the recording folder
    // http://127.0.0.1/index/api/deleteRecordDirectroy?vhost=__defaultVhost__&app=live&stream=ss&period=2020-01-01
    api_regist("/index/api/deleteRecordDirectory", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream");
        auto tuple = MediaTuple{allArgs["vhost"], allArgs["app"], allArgs["stream"], ""};
        auto record_path = Recorder::getRecordPath(Recorder::type_mp4, tuple, allArgs["customized_path"]);
        auto period = allArgs["period"];
        if (!period.empty()) {
            record_path = record_path + period + "/";
        }

        bool recording = false;
        auto name = allArgs["name"];
        if (!name.empty()) {
            // Delete the specified file
            record_path += name;
        } else {
            // Delete the folder, first check if the stream is being recorded
            auto src = MediaSource::find(allArgs["vhost"], allArgs["app"], allArgs["stream"]);
            if (src && src->isRecording(Recorder::type_mp4)) {
                recording = true;
            }
        }
        val["path"] = record_path;
        if (!recording) {
            val["code"] = File::delete_file(record_path, true);
            return;
        }
        File::scanDir(record_path, [](const string &path, bool is_dir) {
            if (is_dir) {
                return true;
            }
            if (path.find("/.") == std::string::npos) {
                File::delete_file(path);
            } else {
                TraceL << "Ignore tmp mp4 file: " << path;
            }
            return true;
        }, true, true);
        File::deleteEmptyDir(record_path);
    });

    api_regist("/index/api/deleteSnapDirectory", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream");
        GET_CONFIG(std::string, root, API::kSnapRoot);
        auto path = File::absolutePath(allArgs["vhost"] + "/" + allArgs["app"] + "/" + allArgs["stream"] + "/" + allArgs["file"], root);
        InfoL << "delete " << path;
        File::delete_file(path, true);
    });

    // Get the list of recording folders or mp4 files
    // http://127.0.0.1/index/api/getMP4RecordFile?vhost=__defaultVhost__&app=live&stream=ss&period=2020-01
    api_regist("/index/api/getMP4RecordFile", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream");
        auto tuple = MediaTuple { allArgs["vhost"], allArgs["app"], allArgs["stream"], "" };
        auto record_path = Recorder::getRecordPath(Recorder::type_mp4, tuple, allArgs["customized_path"]);
        auto period = allArgs["period"];

        // Determine whether to get the mp4 file list or the folder list
        bool search_mp4 = period.size() == sizeof("2020-02-01") - 1;
        if (search_mp4) {
            record_path = record_path + period + "/";
        }

        Json::Value paths(arrayValue);
        // This is to filter the date and get the folder list
        File::scanDir(record_path, [&](const string &path, bool isDir) {
            auto pos = path.rfind('/');
            if (pos != string::npos) {
                string relative_path = path.substr(pos + 1);
                if (search_mp4) {
                    if (!isDir) {
                        // We only collect mp4 files, we are not interested in folders
                        paths.append(relative_path);
                    }
                } else if (isDir && relative_path.find(period) == 0) {
                    // Match the folder for the corresponding date
                    paths.append(relative_path);
                }
            }
            return true;
        }, false);

        val["data"]["rootPath"] = record_path;
        val["data"]["paths"] = paths;
    });

    static auto responseSnap = [](const string &snap_path,
                                  const HttpSession::KeyValue &headerIn,
                                  const HttpSession::HttpResponseInvoker &invoker,
                                  const string &err_msg = "") {
        static bool s_snap_success_once = false;
        StrCaseMap headerOut;
        GET_CONFIG(string, defaultSnap, API::kDefaultSnap);
        if (!File::fileSize(snap_path)) {
            if (!err_msg.empty() && (!s_snap_success_once || defaultSnap.empty())) {
#if 0
                // If the screenshot has never been successful or the default screenshot image is empty, then directly return the FFmpeg error log
                headerOut["Content-Type"] = HttpFileManager::getContentType(".txt");
                invoker.responseFile(headerIn, headerOut, err_msg, false, false);
#endif
                Value val;
                val["code"] = API::Exception;
                val["msg"] = err_msg;
                invoker(404, headerOut, val.toStyledString());
                return;
            }
            // If the screenshot has been successful once, then it is considered that the configuration is error-free, and when the screenshot fails, the preset default image is returned
            const_cast<string &>(snap_path) = File::absolutePath("", defaultSnap);
            headerOut["Content-Type"] = HttpFileManager::getContentType(snap_path.data());
        } else {
            s_snap_success_once = true;
            // The previously generated screenshot file, we default to jpeg format
            headerOut["Content-Type"] = HttpFileManager::getContentType(".jpeg");
        }
        // Return image to http client
        invoker.responseFile(headerIn, headerOut, snap_path);
    };

    // Get screenshot cache or real-time screenshot
    // http://127.0.0.1/index/api/getSnap?url=rtmp://127.0.0.1/record/robot.mp4&timeout_sec=10&expire_sec=3
    api_regist("/index/api/getSnap", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("url", "timeout_sec", "expire_sec");
        GET_CONFIG(string, snap_root, API::kSnapRoot);

        bool have_old_snap = false, res_old_snap = false;
        int expire_sec = allArgs["expire_sec"];
        auto scan_path = File::absolutePath(MD5(allArgs["url"]).hexdigest(), snap_root) + "/";
        string new_snap = StrPrinter << scan_path << time(NULL) << ".jpeg";

        File::scanDir(scan_path, [&](const string &path, bool isDir) {
            if (isDir || !end_with(path, ".jpeg")) {
                // Ignore folders or other types of files
                return true;
            }

            // Find screenshot
            auto tm = findSubString(path.data() + scan_path.size(), nullptr, ".jpeg");
            if (atoll(tm.data()) + expire_sec < time(NULL)) {
                // Screenshot has expired, rename it so that it can be returned when requested again
                rename(path.data(), new_snap.data());
                have_old_snap = true;
                return true;
            }

            // Screenshot exists and has not expired, so return it
            res_old_snap = true;
            responseSnap(path, allArgs.parser.getHeader(), invoker);
            // Interrupt traversal
            return false;
        });

        if (res_old_snap) {
            // Old screenshot has been replied
            return;
        }

        // No screenshot or screenshot has expired
        if (!have_old_snap) {
            // No expired screenshot, generate an empty file, the purpose is to create the folder path by the way
            // At the same time, prevent the FFmpeg process from being started multiple times by continuously trying to call this API during the FFmpeg
            // screenshot generation process
            auto file = File::create_file(new_snap, "wb");
            if (file) {
                fclose(file);
            }
        }

        // Start the FFmpeg process, start taking screenshots, generate temporary files, replace them with formal files after successful screenshots
        auto new_snap_tmp = new_snap + ".tmp";
        FFmpegSnap::makeSnap(allArgs["async"], allArgs["url"], new_snap_tmp, 0, allArgs["timeout_sec"], [invoker, allArgs, new_snap, new_snap_tmp](bool success, const string &err_msg) {
            if (!success) {
                // Screenshot generation failed, there may be residual empty files
                File::delete_file(new_snap_tmp);
            } else {
                // Temporary file changed to formal file
                File::delete_file(new_snap);
                rename(new_snap_tmp.data(), new_snap.data());
            }
            responseSnap(new_snap, allArgs.parser.getHeader(), invoker, err_msg);
        });
    });

    api_regist("/index/api/getStatistic", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        getStatisticJson([headerOut, val, invoker](const Value &data) mutable {
            val["data"] = data;
            invoker(200, headerOut, val.toStyledString());
        });
    });

#ifdef ENABLE_WEBRTC
    api_regist("/index/api/webrtc",[](API_ARGS_STRING_ASYNC){
        CHECK_ARGS("type");
        auto type = allArgs["type"];
        auto offer = allArgs.args;
        CHECK(!offer.empty(), "http body(webrtc offer sdp) is empty");

        auto &session = static_cast<Session&>(sender);
        auto args = std::make_shared<WebRtcArgsImp<std::string>>(allArgs, sender.getIdentifier());
        WebRtcPluginManager::Instance().negotiateSdp(session, type, *args, [invoker, val, offer, headerOut](const WebRtcInterface &exchanger) mutable {
            auto &handler = const_cast<WebRtcInterface &>(exchanger);
            try {
                val["sdp"] = handler.getAnswerSdp(offer);
                val["id"] = exchanger.getIdentifier();
                val["type"] = "answer";
                invoker(200, headerOut, val.toStyledString());
            } catch (std::exception &ex) {
                val["code"] = API::Exception;
                val["msg"] = ex.what();
                invoker(200, headerOut, val.toStyledString());
            }
        });
    });

    static constexpr char delete_webrtc_url[] = "/index/api/delete_webrtc";
    static auto whip_whep_func = [](const char *type, API_ARGS_STRING_ASYNC) {
        auto offer = allArgs.args;
        CHECK(!offer.empty(), "http body(webrtc offer sdp) is empty");

        auto &session = static_cast<Session &>(sender);
        auto location = std::string(session.overSsl() ? "https://" : "http://") + allArgs["host"] + delete_webrtc_url;
        auto args = std::make_shared<WebRtcArgsImp<std::string>>(allArgs, sender.getIdentifier());
        WebRtcPluginManager::Instance().negotiateSdp(session, type, *args, [invoker, offer, headerOut, location](const WebRtcInterface &exchanger) mutable {
            auto &handler = const_cast<WebRtcInterface &>(exchanger);
            try {
                // Set return type
                headerOut["Content-Type"] = "application/sdp";
                headerOut["Location"] = location + "?id=" + exchanger.getIdentifier() + "&token=" + exchanger.deleteRandStr();
                invoker(201, headerOut, handler.getAnswerSdp(offer));
            } catch (std::exception &ex) {
                headerOut["Content-Type"] = "text/plain";
                invoker(406, headerOut, ex.what());
            }
        });
    };

    api_regist("/index/api/whip", [](API_ARGS_STRING_ASYNC) { whip_whep_func("push", API_ARGS_VALUE, invoker); });
    api_regist("/index/api/whep", [](API_ARGS_STRING_ASYNC) { whip_whep_func("play", API_ARGS_VALUE, invoker); });

    api_regist(delete_webrtc_url, [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS("id", "token");
        CHECK(allArgs.parser.method() == "DELETE", "http method is not DELETE: " + allArgs.parser.method());
        auto obj = WebRtcTransportManager::Instance().getItem(allArgs["id"]);
        if (!obj) {
            invoker(404, headerOut, "id not found");
            return;
        }
        if (obj->deleteRandStr() != allArgs["token"]) {
            invoker(401, headerOut, "token incorrect");
            return;
        }
        obj->safeShutdown(SockException(Err_shutdown, "deleted by http api"));
        invoker(200, headerOut, "");
    });

    // Get WebRTCProxyPlayer connection information
    api_regist("/index/api/getWebrtcProxyPlayerInfo", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("key");

        auto player_proxy = s_player_proxy.find(allArgs["key"]);
        if (!player_proxy) {
            throw ApiRetException("Stream proxy not found", API::NotFound);
        }

        auto media_player = player_proxy->getDelegate();
        if (!media_player) {
            throw ApiRetException("Media player not found", API::OtherFailed);
        }

        auto webrtc_player_imp = std::dynamic_pointer_cast<WebRtcProxyPlayerImp>(media_player);
        if (!webrtc_player_imp) {
            throw ApiRetException("Stream proxy is not WebRTC type", API::OtherFailed);
        }

        auto webrtc_transport = webrtc_player_imp->getWebRtcTransport();
        if (!webrtc_transport) {
            throw ApiRetException("WebRTC transport not available", API::OtherFailed);
        }

        std::string stream_key = allArgs["key"];
        webrtc_transport->getTransportInfo([val, headerOut, invoker, stream_key](Json::Value transport_info) mutable {
            transport_info["stream_key"] = stream_key;

            if (transport_info.isMember("error")) {
                Json::Value error_val;
                error_val["code"] = API::OtherFailed;
                error_val["msg"] = transport_info["error"].asString();
                invoker(200, headerOut, error_val.toStyledString());
                return;
            }

            // Return result successfully
            Json::Value success_val;
            success_val["code"] = API::Success;
            success_val["msg"] = "success";
            success_val["data"] = transport_info;
            invoker(200, headerOut, success_val.toStyledString());
        });
    });

    api_regist("/index/api/addWebrtcRoomKeeper",[](API_ARGS_MAP_ASYNC){
        CHECK_SECRET();
        CHECK_ARGS("server_host", "server_port", "room_id", "ssl");
       //server_host: signaling server host
        //server_post: signaling server host
        //room_id: registered id, the signaling server will check the uniqueness of the id
        addWebrtcRoomKeeper(allArgs["server_host"], allArgs["server_port"], allArgs["room_id"], allArgs["ssl"],
            [val, headerOut, invoker](const SockException &ex, const string &key) mutable {
                if (ex) {
                    val["code"] = API::OtherFailed;
                    val["msg"] = ex.what();
                } else {
                    val["msg"] = "success";
                    val["data"]["room_key"] = key;
                }
                invoker(200, headerOut, val.toStyledString());
            });
    });

    api_regist("/index/api/delWebrtcRoomKeeper",[](API_ARGS_MAP_ASYNC){
        CHECK_SECRET();
        CHECK_ARGS("room_key");

        delWebrtcRoomKeeper(allArgs["room_key"],
            [val, headerOut, invoker](const SockException &ex) mutable {
                if (ex) {
                    val["code"] = API::OtherFailed;
                    val["msg"] = ex.what();
                }
                invoker(200, headerOut, val.toStyledString());
            });
    });

    api_regist("/index/api/listWebrtcRoomKeepers", [](API_ARGS_MAP) {
        CHECK_SECRET();
        listWebrtcRoomKeepers([&val](const std::string& key, const WebRtcSignalingPeer::Ptr& p) {
            Json::Value item = ToJson(p);
            item["room_key"] = key;
            val["data"].append(item);
        });
    });

    api_regist("/index/api/listWebrtcRooms", [](API_ARGS_MAP) {
        CHECK_SECRET();
        listWebrtcRooms([&val](const std::string& key, const WebRtcSignalingSession::Ptr& p) {
            Json::Value item = ToJson(p);
            item["room_id"] = key;
            val["data"].append(item);
        });
    });
#endif

#if defined(ENABLE_VERSION)
    api_regist("/index/api/version", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        Value ver;
        ver["buildTime"] = BUILD_TIME;
        ver["branchName"] = BRANCH_NAME;
        ver["commitHash"] = COMMIT_HASH;
        val["data"] = ver;
        invoker(200, headerOut, val.toStyledString());
    });
#endif

#if ENABLE_MP4
    api_regist("/index/api/loadMP4File", [](API_ARGS_MAP) {
        CHECK_SECRET();
        CHECK_ARGS("vhost", "app", "stream", "file_path");

        ProtocolOption option;
        // mp4 supports multiple tracks
        option.max_track = 16;
        // By default, demultiplexing mp4 does not generate mp4
        option.enable_mp4 = false;
        // But if the parameter explicitly specifies to enable mp4 then it is also allowed
        option.load(allArgs);
        // Force automatic shutdown when no one is watching
        option.auto_close = true;
        auto tuple = MediaTuple { allArgs["vhost"], allArgs["app"], allArgs["stream"], "" };
        auto reader = std::make_shared<MP4Reader>(tuple, allArgs["file_path"], option);
        // sample_ms is set to 0, loaded from the configuration file; file_repeat can be specified, if the configuration file also specifies loop demultiplexing, then force it to be enabled
        reader->startReadMP4(0, true, allArgs["file_repeat"]);
        auto seek_ms = allArgs["seek_ms"].as<uint32_t>();
        auto speed = allArgs["speed"].as<float>();
        if (seek_ms || speed) {
            auto p = static_pointer_cast<MediaSourceEvent>(reader);
            p->getOwnerPoller(MediaSource::NullMediaSource())->async([seek_ms, speed, p]() {
                if (seek_ms) {
                    p->seekTo(MediaSource::NullMediaSource(), seek_ms);
                }
                if (speed && speed != 1.0) {
                    p->speed(MediaSource::NullMediaSource(), speed);
                }
            });
        }
        val["data"]["duration_ms"] = (Json::UInt64)reader->getDemuxer()->getDurationMS();
    });
#endif

    GET_CONFIG_FUNC(std::set<std::string>, download_roots, API::kDownloadRoot, [](const string &str) -> std::set<std::string> {
        std::set<std::string> ret;
        auto vec = toolkit::split(str, ";");
        for (auto &item : vec) {
            auto root = File::absolutePath("", item, true);
            ret.emplace(std::move(root));
        }
        return ret;
    });

    api_regist("/index/api/downloadFile", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS("file_path");
        auto file_path = allArgs["file_path"];

        if (file_path.find("..") != std::string::npos) {
            invoker(401, StrCaseMap {}, "You can not access parent directory");
            return;
        }
        bool safe = false;
        for (auto &root : download_roots) {
            if (start_with(file_path, root)) {
                safe = true;
                break;
            }
        }
        if (!safe) {
            invoker(401, StrCaseMap {}, "You can not download files outside the root directory");
            return;
        }

        // File download authentication is completed through on_http_access. Please make sure that the access authentication URL parameters and the access file path are legal
        HttpSession::HttpAccessPathInvoker file_invoker = [allArgs, invoker](const string &err_msg, const string &cookie_path_in, int life_second) mutable {
            if (!err_msg.empty()) {
                invoker(401, StrCaseMap {}, err_msg);
            } else {
                StrCaseMap res_header;
                auto save_name = allArgs["save_name"];
                if (!save_name.empty()) {
                    res_header.emplace("Content-Disposition", "attachment;filename=\"" + save_name + "\"");
                }
                invoker.responseFile(allArgs.parser.getHeader(), res_header, allArgs["file_path"]);
            }
        };

        bool flag = NOTICE_EMIT(BroadcastHttpAccessArgs, Broadcast::kBroadcastHttpAccess, allArgs.parser, file_path, false, file_invoker, sender);
        if (!flag) {
            // No one is listening to the file download authentication event, download is not allowed
            invoker(401, StrCaseMap {}, "None http access event listener");
        }
    });


    api_regist("/index/api/searchOnvifDevice",[](API_ARGS_MAP_ASYNC){
       CHECK_SECRET();
       CHECK_ARGS("timeout_ms");

       auto result = std::make_shared<Value>(std::move(val));
       auto complete_token = std::make_shared<onceToken>(nullptr, [result, headerOut, invoker]() {
           invoker(200, headerOut, result->toStyledString());
       });
       auto lam_search = [complete_token, result](const std::map<string, string> &device_info,
                                                  const std::string &onvif_url) {
           Value obj;
           obj["onvif_url"] = onvif_url;
           for (auto &pr : device_info) {
               obj[pr.first] = pr.second;
           }
           (*result)["data"].append(std::move(obj));
           //Continue to wait for scanning
           return true;
       };
       OnvifSearcher::Instance().sendSearchBroadcast(std::move(lam_search), allArgs["timeout_ms"]);
   });

    api_regist("/index/api/getStreamUrl", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("onvif_url");

        SoapUtil::asyncGetStreamUri(allArgs["onvif_url"],[val, headerOut, allArgs, invoker]
                (const SoapErr &err, const SoapUtil::GetStreamUriRetryInvoker &retry_invoker,
                 int retry_count, const std::string &url) mutable {
            if (err && retry_count == 0 && !allArgs["user_name"].empty() /* &&
                (err.httpCode() == 400 || err.httpCode() == 401)*/) {
                //It failed for the first time, and the user password was provided, and it was determined that the authentication failed.
                retry_invoker(allArgs["user_name"], allArgs["passwd"]);
                return;
            }
            val["code"] = err ? API::OtherFailed : API::Success;
            if (err) {
                val["http_code"] = err.httpCode();
                val["msg"] = (string) err;
            } else {
                val["url"] = url;
            }
            invoker(200, headerOut, val.toStyledString());
        });
    });

#if defined(ENABLE_VIDEOSTACK) && defined(ENABLE_X264) && defined(ENABLE_FFMPEG)
    VideoStackManager::Instance().loadBgImg("novideo.yuv");
    NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastStreamNoneReader, [](BroadcastStreamNoneReaderArgs) {
        auto id = sender.getMediaTuple().stream;
        VideoStackManager::Instance().stopVideoStack(id);
    });

    api_regist("/index/api/stack/start", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        int ret = 0;
        try {
            ret = VideoStackManager::Instance().startVideoStack(allArgs.args);
            val["code"] = ret;
            val["msg"] = ret ? "failed" : "success";
        } catch (const std::exception &e) {
            val["code"] = -1;
            val["msg"] = e.what();
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/index/api/stack/reset", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        int ret = 0;
        try {
            auto ret = VideoStackManager::Instance().resetVideoStack(allArgs.args);
            val["code"] = ret;
            val["msg"] = ret ? "failed" : "success";
        } catch (const std::exception &e) {
            val["code"] = -1;
            val["msg"] = e.what();
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/index/api/stack/stop", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("id");
        auto ret = VideoStackManager::Instance().stopVideoStack(allArgs["id"]);
        val["code"] = ret;
        val["msg"] = ret ? "failed" : "success";
        invoker(200, headerOut, val.toStyledString());
    });
#endif
    /////////////////////////S3MediaKit - MediaServer////////////////////////////

    // Admin api format: /media/api/... 
    api_regist("/media/api/getSyncStatus", [](API_ARGS_MAP_ASYNC) {
        auto on_access = [&sender, headerOut, allArgs, val, invoker]() mutable {
            GET_CONFIG(string, mediaServerId, General::kMediaServerId);

            // 1. transaction_sequence — local cursors (what we have received from each peer)
            auto seq_impl = std::make_shared<TransactionSequenceImp>();
            auto seq_list = seq_impl->findAll();
            Json::Value seq_json = Json::arrayValue;
            for (const auto &s : seq_list) {
                Json::Value item;
                item["peer_guid"] = s.peer_guid;
                item["db_guid"]   = s.db_guid;
                item["sequence"]  = s.sequence;
                seq_json.append(item);
            }
            val["data"]["transaction_sequence"] = seq_json;

            // 2. transaction_peer_ack_log — how far each peer has pulled from us
            auto ack_impl = std::make_shared<PeerAckLogImp>();
            auto ack_list = ack_impl->findAll();
            Json::Value ack_json = Json::arrayValue;
            for (const auto &a : ack_list) {
                Json::Value item;
                item["peer_guid"]     = a.peer_guid;
                item["db_guid"]       = a.db_guid;
                item["src_peer_guid"] = a.src_peer_guid;
                item["src_db_guid"]   = a.src_db_guid;
                item["acked_seq"]     = a.acked_seq;
                item["updated_at"]    = (Json::Int64)a.updated_at;
                ack_json.append(item);
            }
            val["data"]["transaction_ack_log"] = ack_json;

            // 3. transaction_log row count per (peer_guid, db_guid)
            // Use cursors already fetched from transaction_sequence
            Json::Value log_count_json = Json::arrayValue;
            auto log_impl = std::make_shared<TransactionLogImp>();
            for (const auto &s : seq_list) {
                // Count rows with sequence > 0 (i.e., all) for this peer/db pair
                auto rows_for_peer = log_impl->findSinceSeq(s.peer_guid, s.db_guid, 0, 0);
                Json::Value lc;
                lc["peer_guid"] = s.peer_guid;
                lc["db_guid"]   = s.db_guid;
                lc["count"]     = (Json::UInt)rows_for_peer.size();
                log_count_json.append(lc);
            }
            val["data"]["transaction_log_counts"] = log_count_json;
            
            // 4. local node identity
            auto imp = std::make_shared<MiscDataImp>();
            auto ret = imp->findAll();
            Json::Value misc_json;
            for (const auto &r : ret) {
                if (r.key.empty() || r.value.empty()) {
                    continue;
                }
                val["data"][r.key] = r.value;
            }
            val["data"]["mediaServerId"] = mediaServerId;
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_CLUSTER_AUTHOR_ASYNC(on_access);
    });

    api_regist("/media/api/cluster/access", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS("secret", "authorId", "mediaServerId");

        string secret = allArgs["secret"];
        string authorId = allArgs["authorId"];
        string mediaServerId = allArgs["mediaServerId"];

        // Validate: chỉ cho phép các peer đã đăng ký trong ClusterManager được gọi đến xác thực
        auto peerIds = ClusterManager::Instance().getMediaServerIds();
        bool allowed = false;
        for (auto &pr : peerIds) {
            if (pr == mediaServerId) { allowed = true; break; }
        }

        if (!allowed) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_PERMISSION_DENIED, "Media server not allowed");
            return;
        }

        GET_CONFIG(string, mediaServerId_, General::kMediaServerId)
        if (api_secret != secret || mediaServerId_ != authorId) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_UNAUTHORIZED, "Unauthorized");
            return;
        }

        val["data"]["msg"] = "Authorized";
        invoker(200, headerOut, val.toStyledString());
    });

    // List files/folders in a directory under api.downloadRoot
    // GET /media/api/listFiles?secret=xxx&path=relative/path
    api_regist("/media/api/listFiles", [](API_ARGS_MAP) {
        CHECK_SECRET();

        GET_CONFIG_FUNC(std::string, download_root_str, API::kDownloadRoot, [](const string &str) -> std::string {
            return str;
        });
        auto root_abs = File::absolutePath("", download_root_str, true);
        // Remove trailing slash for consistent concat
        if (!root_abs.empty() && root_abs.back() == '/') {
            root_abs.pop_back();
        }

        string rel_path = allArgs["path"];
        // Reject traversal attempts
        if (rel_path.find("..") != std::string::npos) {
            val["code"] = API::OtherFailed;
            val["msg"] = "Path traversal not allowed";
            return;
        }
        // Strip leading slash
        if (!rel_path.empty() && rel_path.front() == '/') {
            rel_path = rel_path.substr(1);
        }

        string dir_path = rel_path.empty() ? root_abs : (root_abs + "/" + rel_path);

        // Verify directory exists
        struct stat st;
        if (::stat(dir_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            val["code"] = API::OtherFailed;
            val["msg"] = "Directory not found";
            return;
        }

        Json::Value entries(Json::arrayValue);
        File::scanDir(dir_path, [&](const string &entry_path, bool isDir) -> bool {
            // Only immediate children (non-recursive)
            string inside = entry_path.substr(dir_path.size());
            if (inside.empty()) return true;
            if (inside.front() == '/') inside = inside.substr(1);
            if (inside.find('/') != std::string::npos) return true; // deeper level, skip

            Json::Value item;
            item["name"] = inside;
            item["isDir"] = isDir;
            if (!isDir) {
                struct stat fs;
                item["size"] = (::stat(entry_path.c_str(), &fs) == 0) ? (Json::Int64)fs.st_size : (Json::Int64)-1;
                item["mtime"] = (::stat(entry_path.c_str(), &fs) == 0) ? (Json::Int64)fs.st_mtime : (Json::Int64)0;
            } else {
                item["size"] = (Json::Int64)-1;
                item["mtime"] = (Json::Int64)0;
            }
            item["path"] = rel_path.empty() ? inside : (rel_path + "/" + inside);
            item["absPath"] = entry_path;
            entries.append(item);
            return true;
        }, false);

        val["data"]["root"] = root_abs;
        val["data"]["path"] = rel_path;
        val["data"]["entries"] = entries;
    });

    // Download a file under api.downloadRoot (secret-auth, no on_http_access needed)
    // GET /media/api/serveFile?secret=xxx&path=relative/path&save_name=foo.txt
    api_regist("/media/api/serveFile", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS("path");

        GET_CONFIG_FUNC(std::string, download_root_str2, API::kDownloadRoot, [](const string &str) -> std::string {
            return str;
        });
        auto root_abs2 = File::absolutePath("", download_root_str2, true);
        if (!root_abs2.empty() && root_abs2.back() == '/') {
            root_abs2.pop_back();
        }

        string rel = allArgs["path"];
        if (rel.find("..") != std::string::npos) {
            invoker(401, StrCaseMap{}, "Path traversal not allowed");
            return;
        }
        if (!rel.empty() && rel.front() == '/') {
            rel = rel.substr(1);
        }

        string abs_path = rel.empty() ? root_abs2 : (root_abs2 + "/" + rel);

        struct stat st;
        if (::stat(abs_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
            invoker(404, StrCaseMap{}, "File not found");
            return;
        }

        StrCaseMap res_header;
        auto save_name = allArgs["save_name"];
        if (!save_name.empty()) {
            res_header.emplace("Content-Disposition", "attachment;filename=\"" + save_name + "\"");
        }
        invoker.responseFile(allArgs.parser.getHeader(), res_header, abs_path);
    });

    api_regist("/media/api/getThreadsLoad", [](API_ARGS_MAP_ASYNC) {
        auto on_access = [&sender, headerOut, allArgs, val, invoker]() mutable {
            getThreadsLoad(EventPollerPool::Instance(), API_ARGS_VALUE, invoker);
        };
        CHECK_CLUSTER_AUTHOR_ASYNC(on_access);
    });

    api_regist("/media/api/getWorkThreadsLoad", [](API_ARGS_MAP_ASYNC) {
        auto on_access = [&sender, headerOut, allArgs, val, invoker]() mutable {
            getThreadsLoad(WorkThreadPool::Instance(), API_ARGS_VALUE, invoker);
        };
        CHECK_CLUSTER_AUTHOR_ASYNC(on_access);
    });

    api_regist("/media/api/getStatistic", [](API_ARGS_MAP_ASYNC) {
        auto on_access = [&sender, headerOut, allArgs, val, invoker]() mutable {
            getStatisticJson([headerOut, val, invoker](const Value &data) mutable {
                val["data"] = data;
                invoker(200, headerOut, val.toStyledString());
            });
        };
       
        CHECK_CLUSTER_AUTHOR_ASYNC(on_access);
    });

    api_regist("/media/api/systemStatistic", [](API_ARGS_MAP_ASYNC) {
        auto on_access = [&sender, headerOut, allArgs, val, invoker]() mutable {
            val["data"] = GlobalMonitor::Instance().makeSystemStatisticJson();
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_CLUSTER_AUTHOR_ASYNC(on_access);
    });

    api_regist("/media/api/systemStatisticHistory", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("from", "to", "limit");
        auto on_access = [&sender, headerOut, allArgs, val, invoker]() mutable {
            int64_t from_ts = allArgs["from"];
            int64_t to_ts   = allArgs["to"];
            int     limit   = allArgs["limit"];
            int     bucket_sec = allArgs["bucket_sec"];
            val["data"] = GlobalMonitor::Instance().getSystemStatisticHistory(from_ts, to_ts, limit, bucket_sec);
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_CLUSTER_AUTHOR_ASYNC(on_access);
    });

    // List statistics for all camera devices
    // GET /media/api/device/statisticsList?secret=xxx
    api_regist("/media/api/device/statisticsList", [](API_ARGS_MAP) {
        CHECK_SECRET();

        val["data"] = Json::arrayValue;
        DeviceSource::for_each_device([&](const DeviceSource::Ptr &device) {
            auto item = makeDeviceStatisticJson(device);
            if (!item.isNull() && item.isMember("deviceId")) {
                val["data"].append(item);
            }
        });
    });

    api_regist("/media/api/storage/dashboard/detail", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        val["data"] = TierStorageManager::Instance().getDashboardDetail();
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/dashboard/summary", [](API_ARGS_MAP) {
        CHECK_SECRET();
        val["data"] = apiDashboardSummaryToJson();
    });

    api_regist("/media/api/storage/alert/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        std::string level = allArgs["level"];
        int acknowledged = allArgs["acknowledged"].empty() ? -1 : std::stoi(allArgs["acknowledged"]);
        int page = allArgs["page"].empty() ? 0 : std::stoi(allArgs["page"]);
        int size = allArgs["size"].empty() ? 20 : std::stoi(allArgs["size"]);
        if (size <= 0 || size > 100) size = 20;

        StorageAlertImp imp;
        Json::Value items(Json::arrayValue);
        auto pool_map = apiLoadStoragePoolMap();
        for (const auto &a : imp.query(level, acknowledged, page, size)) {
            items.append(apiAlertToJson(a, pool_map));
        }
        val["data"]["items"] = items;
        val["data"]["page"] = page;
        val["data"]["size"] = size;
        val["data"]["total"] = imp.countQuery(level, acknowledged);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/restoreJob/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        std::string camera_id = allArgs["camera_id"];
        std::string status = allArgs["status"];
        int64_t from_time = allArgs["from_time"].empty() ? 0 : std::stoll(allArgs["from_time"]);
        int64_t to_time = allArgs["to_time"].empty() ? 0 : std::stoll(allArgs["to_time"]);
        int page = allArgs["page"].empty() ? 0 : std::stoi(allArgs["page"]);
        int size = allArgs["size"].empty() ? 20 : std::stoi(allArgs["size"]);
        if (size <= 0 || size > 100) size = 20;

        Json::Value items(Json::arrayValue);
        for (const auto &j : TierStorageManager::Instance().listRestoreJobs(camera_id, status, from_time, to_time, page, size)) {
            items.append(apiRestoreJobToJson(j));
        }
        val["data"]["items"] = items;
        val["data"]["page"] = page;
        val["data"]["size"] = size;
        val["data"]["total"] = TierStorageManager::Instance().countRestoreJobs(camera_id, status, from_time, to_time);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/tieringJob/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        std::string status = allArgs["status"];
        std::string camera_id = allArgs["camera_id"];
        std::string source_tier = allArgs["source_tier"];
        std::string target_tier = allArgs["target_tier"];
        int64_t from_time = allArgs["from_time"].empty() ? 0 : std::stoll(allArgs["from_time"]);
        int64_t to_time = allArgs["to_time"].empty() ? 0 : std::stoll(allArgs["to_time"]);
        int page = allArgs["page"].empty() ? 0 : std::stoi(allArgs["page"]);
        int size = allArgs["size"].empty() ? 20 : std::stoi(allArgs["size"]);
        if (size <= 0 || size > 100) size = 20;

        Json::Value items(Json::arrayValue);
        for (const auto &j : TierStorageManager::Instance().listTieringJobs(camera_id, status, source_tier, target_tier, from_time, to_time, page, size)) {
            items.append(apiTieringJobToJson(j));
        }
        val["data"]["items"] = items;
        val["data"]["page"] = page;
        val["data"]["size"] = size;
        val["data"]["total"] = TierStorageManager::Instance().countTieringJobs(camera_id, status, from_time, to_time);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/expiredSegment/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        std::string camera_id = allArgs["camera_id"];
        int64_t from_time = allArgs["from_time"].empty() ? 0 : std::stoll(allArgs["from_time"]);
        int64_t to_time = allArgs["to_time"].empty() ? 0 : std::stoll(allArgs["to_time"]);
        int page = allArgs["page"].empty() ? 0 : std::stoi(allArgs["page"]);
        int size = allArgs["size"].empty() ? 20 : std::stoi(allArgs["size"]);
        if (size <= 0 || size > 100) size = 20;

        SegmentTierRangeImp imp;
        Json::Value items(Json::arrayValue);
        for (const auto &r : imp.findExpired(camera_id, from_time, to_time, page, size)) {
            items.append(apiExpiredSegmentToJson(r));
        }
        val["data"]["items"] = items;
        val["data"]["page"] = page;
        val["data"]["size"] = size;
        val["data"]["total"] = imp.countExpired(camera_id, from_time, to_time);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/protected/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        std::string camera_id = allArgs["camera_id"];
        std::string type = allArgs["type"];
        int64_t from_time = allArgs["from_time"].empty() ? 0 : std::stoll(allArgs["from_time"]);
        int64_t to_time = allArgs["to_time"].empty() ? 0 : std::stoll(allArgs["to_time"]);
        int page = allArgs["page"].empty() ? 0 : std::stoi(allArgs["page"]);
        int size = allArgs["size"].empty() ? 20 : std::stoi(allArgs["size"]);
        if (size <= 0 || size > 100) size = 20;

        ProtectedVideoImp imp;
        Json::Value items(Json::arrayValue);
        for (const auto &p : imp.query(camera_id, type, from_time, to_time, page, size)) {
            items.append(apiProtectedVideoToJson(p));
        }
        val["data"]["items"] = items;
        val["data"]["page"] = page;
        val["data"]["size"] = size;
        val["data"]["total"] = imp.countQuery(camera_id, type, from_time, to_time);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/pool/options", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        val["data"]["poolsTypeSupport"] = Json::objectValue;
        val["data"]["poolsTypeSupport"]["HOT"] = poolTypeSupport(TierType::HotTier);
        val["data"]["poolsTypeSupport"]["WARM"] = poolTypeSupport(TierType::WarmTier);
        val["data"]["poolsTypeSupport"]["COLD"] = poolTypeSupport(TierType::ColdTier);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/mountpoint/available", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();

        std::string storage_types = allArgs["include"];
        auto available_mount_points = TierStorageManager::Instance().getAvailableMountPoints(storage_types);

        Json::Value data;
        data["mount_point"] = Json::arrayValue;
        for (const auto &mp : available_mount_points) {
            data["mount_point"].append(mp.toJson());
        }
        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/pool/testConnection", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();

        std::string pool_id = allArgs["pool_id"];
        StoragePool pool;
        if (!pool_id.empty()) {
            auto existing = TierStorageManager::Instance().getPool(pool_id);
            if (existing.empty()) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
                return;
            }
            pool = existing.front();
        } else {
            pool = apiPoolFromJson(allArgs.getArgs());
        }

        std::string message;
        int latency_ms = 0;
        bool ok = TierStorageManager::Instance().testPoolConnection(pool, message, latency_ms);

        Json::Value data;
        data["status"] = ok ? "OK" : "ERROR";
        data["latency_ms"] = latency_ms;
        data["can_read"] = ok;
        data["can_write"] = ok;
        data["message"] = message;
        val["data"] = data;
        if (!ok) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_CONN_FAILED, message);
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/camera/effectivePolicy/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();

        std::string search = allArgs["search"];
        Json::Value items(Json::arrayValue);
        DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
            auto device_tuple = src->getDeviceTuple();
            if (!search.empty() && device_tuple.name.find(search) == std::string::npos) {
                return;
            }
            auto result = TierStorageManager::Instance().getEffectivePolicy(device_tuple.device_id);
            Json::Value item;
            item["camera_id"] = result.camera_id;
            item["camera_name"] = device_tuple.name;
            item["policy_id"] = result.policy_id;
            item["policy_name"] = result.policy_name;
            item["source"] = result.source;
            item["source_id"] = result.source == policySourceToString(PolicySource::CAMERA) ? result.camera_id : "";
            item["allow_camera_override"] = result.allow_camera_override;
            items.append(item);
        }, GENERIC_RTSP_CAMERA_SCHEMA);

        val["data"]["total"] = static_cast<int>(items.size());
        val["data"]["items"] = items;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/camera/timeline", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("camera_id", "start_time", "end_time");

        std::string camera_id = allArgs["camera_id"];
        int64_t start_time = allArgs["start_time"];
        int64_t end_time = allArgs["end_time"];
        bool include_deleted = allArgs["include_deleted"];

        if (start_time <= 0 || end_time <= 0 || start_time >= end_time) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_TIME_RANGE, "start_time and end_time must be valid");
            return;
        }

        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }
        auto device_tuple = device->getDeviceTuple();

        Json::Value ranges_json(Json::arrayValue);
        auto pool_map = apiLoadStoragePoolMap();
        auto ranges = TierStorageManager::Instance().getCameraTimeline(camera_id, start_time, end_time);
        for (const auto &r : ranges) {
            if (!include_deleted && r.status == segmentStatusToString(SegmentStatus::DELETED)) {
                continue;
            }
            Json::Value range;
            range["start"] = static_cast<Json::Int64>(r.start);
            range["end"] = static_cast<Json::Int64>(r.end);
            range["tier"] = r.tier;
            range["pool_id"] = r.pool_id;
            range["status"] = r.status;
            range["segment_count"] = static_cast<Json::Int64>(r.segment_count);
            range["size_bytes"] = static_cast<Json::Int64>(r.size_bytes);
            range["restore_required"] = apiIsRestoreRequiredPool(r.pool_id, pool_map);
            range["has_motion"] = r.has_motion;
            range["has_event"] = r.has_event;
            ranges_json.append(range);
        }

        auto effective = TierStorageManager::Instance().getEffectivePolicy(camera_id);
        val["data"]["camera_id"] = camera_id;
        val["data"]["camera_name"] = device_tuple.name;
        val["data"]["policy_id"] = effective.policy_id;
        val["data"]["policy_name"] = effective.policy_name;
        val["data"]["ranges"] = ranges_json;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/camera/summary", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("camera_id");

        std::string camera_id = allArgs["camera_id"];
        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }
        auto device_tuple = device->getDeviceTuple();

        auto summary = TierStorageManager::Instance().getCameraStorageSummary(camera_id);
        auto effective = TierStorageManager::Instance().getEffectivePolicy(camera_id);

        Json::Value data;
        data["camera_id"] = summary.camera_id;
        data["camera_name"] = device_tuple.name;
        data["policy_id"] = effective.policy_id;
        data["policy_name"] = effective.policy_name;
        data["policy_source"] = effective.source;
        data["total_size_bytes"] = static_cast<Json::Int64>(summary.total_used_bytes);
        data["total_segment_count"] = static_cast<Json::Int64>(summary.total_segments);

        Json::Value tiers_arr(Json::arrayValue);
        for (const auto &t : summary.tiers) {
            Json::Value tv;
            tv["tier"] = t.tier;
            tv["from_time"] = static_cast<Json::Int64>(t.oldest_segment_time);
            tv["to_time"] = static_cast<Json::Int64>(t.newest_segment_time);
            tv["size_bytes"] = static_cast<Json::Int64>(t.used_bytes);
            tv["segment_count"] = static_cast<Json::Int64>(t.segment_count);
            tiers_arr.append(tv);
        }
        data["tier_summary"] = tiers_arr;
        data["last_tiering_job_time"] = static_cast<Json::Int64>(apiLatestTieringJobTime(camera_id));
        data["status"] = "OK";

        val["data"] = data;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/pool/list", [](API_ARGS_MAP) {
        CHECK_SECRET();
        std::string tier = allArgs["tier"];
        std::string type = allArgs["type"];
        std::string status = allArgs["status"];
        std::string keyword = allArgs["keyword"];
        val["data"] = Json::arrayValue;
        for (const auto &p : TierStorageManager::Instance().listPools(tier, type, keyword)) {
            const auto pool_status = p.health_status.empty() ? "OK" : p.health_status;
            if (!status.empty() && pool_status != status) continue;
            val["data"].append(apiPoolDetailToJson(p));
        }
    });

    api_regist("/media/api/storage/pool/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("id");
        auto pools = TierStorageManager::Instance().getPool(allArgs["id"]);
        if (pools.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
            return;
        }
        val["data"] = apiPoolDetailToJson(pools.front());
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/pool/create", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("name", "type", "tier");
        auto pool = apiPoolFromJson(allArgs.getArgs());
        std::string err;
        if (!apiValidateStoragePoolByType(pool, err)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, err);
            return;
        }
        if (pool.high_watermark_percent >= pool.critical_watermark_percent) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_WATERMARK_PERCENT, "high_watermark_percent must be less than critical_watermark_percent");
            return;
        }
        auto id = TierStorageManager::Instance().createPool(pool);
        if (id.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_CREATE_FAILED, "Failed to create storage pool");
            return;
        }
        val["data"]["id"] = id;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/pool/update", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("id");
        auto body = allArgs.getArgs();
        std::string pool_id = body["id"].asString();
        auto existing = TierStorageManager::Instance().getPool(pool_id);
        if (existing.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_NOT_FOUND, "Storage pool not found");
            return;
        }
        auto base = existing.front();
        if (body.isMember("name") && !body["name"].asString().empty()) base.name = body["name"].asString();
        if (body.isMember("type") && !body["type"].asString().empty()) base.type = body["type"].asString();
        if (body.isMember("tier") && !body["tier"].asString().empty()) base.tier = body["tier"].asString();
        if (body.isMember("enabled")) base.enabled = body["enabled"].asBool() ? 1 : 0;
        if (body.isMember("health_check_enabled")) base.health_check_enabled = body["health_check_enabled"].asBool() ? 1 : 0;
        if (body.isMember("high_watermark_percent")) base.high_watermark_percent = body["high_watermark_percent"].asInt();
        if (body.isMember("critical_watermark_percent")) base.critical_watermark_percent = body["critical_watermark_percent"].asInt();
        auto patch = apiPoolFromJson(body);
        if (patch.endpoint.has_value()) base.endpoint = patch.endpoint;
        if (patch.bucket.has_value()) base.bucket = patch.bucket;
        if (patch.base_path.has_value()) base.base_path = patch.base_path;
        if (patch.access_key.has_value()) base.access_key = patch.access_key;
        if (patch.secret_key_enc.has_value()) base.secret_key_enc = patch.secret_key_enc;
        if (patch.mount_path.has_value()) base.mount_path = patch.mount_path;
        if (patch.network_path.has_value()) base.network_path = patch.network_path;

        std::string err;
        if (!apiValidateStoragePoolByType(base, err)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, err);
            return;
        }
        if (base.high_watermark_percent >= base.critical_watermark_percent) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_WATERMARK_PERCENT, "high_watermark_percent must be less than critical_watermark_percent");
            return;
        }
        if (!TierStorageManager::Instance().updatePool(base)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_UPDATE_FAILED, "Failed to update storage pool");
            return;
        }
        val["data"]["id"] = pool_id;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/pool/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("id");
        int ref_count = 0;
        bool is_default_pool = false;
        if (!TierStorageManager::Instance().deletePool(allArgs["id"], ref_count, is_default_pool)) {
            val["data"]["ref_count"] = ref_count;
            val["data"]["is_default_pool"] = is_default_pool;
            if (is_default_pool) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_DEFAULT_CANNOT_DELETE, "Cannot delete system default storage pool");
                return;
            }
            if (ref_count > 0) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_IN_USE, "Storage pool is used by " + std::to_string(ref_count) + " policies");
                return;
            }
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_DELETE_FAILED, "Failed to delete storage pool");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        std::string keyword = allArgs["keyword"];
        int enabled_filter = !allArgs["enabled"].empty() ? allArgs["enabled"].as<int>() : -1;
        int page = !allArgs["page"].empty() ? allArgs["page"].as<int>() : 0;
        int size = !allArgs["size"].empty() ? allArgs["size"].as<int>() : 20;
        if (size <= 0 || size > 100) size = 20;

        PolicyAssignmentImp assignment_imp;
        Json::Value items(Json::arrayValue);
        for (const auto &p : TierStorageManager::Instance().listPolicies(keyword, enabled_filter, page, size)) {
            items.append(apiPolicySummaryToJson(p, static_cast<int>(assignment_imp.findCamerasByPolicyId(p.id).size())));
        }
        val["data"]["items"] = items;
        val["data"]["page"] = page;
        val["data"]["size"] = size;
        val["data"]["total"] = TierStorageManager::Instance().countPolicies(keyword, enabled_filter);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("id");
        auto policies = TierStorageManager::Instance().getPolicy(allArgs["id"]);
        if (policies.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }
        val["data"] = apiPolicyDetailToJson(policies.front(), apiLoadStoragePoolMap());
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/create", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("name", "total_retention_days");
        auto policy = apiPolicyFromJson(allArgs.getArgs());
        std::string err;
        if (!apiValidateStoragePolicy(policy, err)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, err);
            return;
        }
        auto id = TierStorageManager::Instance().createPolicy(policy);
        if (id.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_CREATE_FAILED, "Failed to create storage policy");
            return;
        }
        val["data"]["id"] = id;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/update", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("id");
        std::string policy_id = allArgs["id"];
        if (TierStorageManager::Instance().getPolicy(policy_id).empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }
        auto policy = apiPolicyFromJson(allArgs.getArgs());
        std::string err;
        if (!apiValidateStoragePolicy(policy, err)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, err);
            return;
        }
        if (!TierStorageManager::Instance().updatePolicy(policy)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_UPDATE_FAILED, "Failed to update storage policy");
            return;
        }
        val["data"]["id"] = policy_id;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("id");
        std::string policy_id = allArgs["id"];
        if (TierStorageManager::Instance().getPolicy(policy_id).empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }
        int camera_count = 0;
        bool is_default_policy = false;
        if (!TierStorageManager::Instance().deletePolicy(policy_id, camera_count, is_default_policy)) {
            val["data"]["camera_count"] = camera_count;
            val["data"]["is_default_policy"] = is_default_policy;
            if (is_default_policy) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_DEFAULT_CANNOT_DELETE, "Cannot delete system default storage policy");
                return;
            }
            if (camera_count > 0) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_IN_USE, "Policy is applied to " + std::to_string(camera_count) + " cameras");
                return;
            }
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_DELETE_FAILED, "Policy not found or could not be deleted");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/assignCamera", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("camera_id", "policy_id");
        std::string camera_id = allArgs["camera_id"];
        std::string policy_id = allArgs["policy_id"];
        auto device = findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }
        if (TierStorageManager::Instance().getPolicy(policy_id).empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }
        if (!TierStorageManager::Instance().assignPolicyToCamera(camera_id, policy_id, allArgs["override_reason"])) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_ASSIGN_CAMERA_FAILED, "Assign policy to camera failed");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/assignCameras", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("policy_id");
        std::string policy_id = allArgs["policy_id"];
        if (TierStorageManager::Instance().getPolicy(policy_id).empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_NOT_FOUND, "Storage policy not found");
            return;
        }
        Json::Value camera_ids_arr = allArgs.getArgs()["camera_ids"];
        if (camera_ids_arr.empty() || !camera_ids_arr.isArray()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "camera_ids must be an array");
            return;
        }
        std::vector<std::string> camera_ids;
        Json::Value failed_arr(Json::arrayValue);
        for (const auto &c : camera_ids_arr) {
            auto camera_id = c.asString();
            if (findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA)) {
                camera_ids.push_back(camera_id);
            } else {
                failed_arr.append(camera_id);
            }
        }
        std::vector<std::string> failed;
        int assigned = TierStorageManager::Instance().assignPolicyToCameras(camera_ids, policy_id, failed);
        for (const auto &id : failed) failed_arr.append(id);
        val["data"]["assigned_count"] = assigned;
        val["data"]["failed_ids"] = failed_arr;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/removeCamera", [](API_ARGS_MAP_ASYNC) {
        CHECK_SECRET();
        CHECK_ARGS_("camera_id");
        std::string camera_id = allArgs["camera_id"];
        if (!findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }
        if (!TierStorageManager::Instance().removeCameraOverride(camera_id)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POLICY_UNASSIGN_CAMERA_FAILED, "Failed to unassign storage policy from camera");
            return;
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/api/storage/policy/removeCameras", [](API_ARGS_JSON_ASYNC) {
        CHECK_SECRET();
        Json::Value camera_ids_arr = allArgs.getArgs()["camera_ids"];
        if (camera_ids_arr.empty() || !camera_ids_arr.isArray()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "camera_ids must be an array");
            return;
        }
        std::vector<std::string> camera_ids;
        Json::Value failed_arr(Json::arrayValue);
        for (const auto &c : camera_ids_arr) {
            auto camera_id = c.asString();
            if (findDeviceSource(camera_id, GENERIC_RTSP_CAMERA_SCHEMA)) {
                camera_ids.push_back(camera_id);
            } else {
                failed_arr.append(camera_id);
            }
        }
        std::vector<std::string> failed;
        int removed = TierStorageManager::Instance().removeCamerasOverride(camera_ids, failed);
        for (const auto &id : failed) failed_arr.append(id);
        val["data"]["removed_count"] = removed;
        val["data"]["failed_ids"] = failed_arr;
        invoker(200, headerOut, val.toStyledString());
    });

    // Configuration APIs
    managerkit::registerConfigurationApis();
    // Playback APIs
    managerkit::registerPlaybackApis();
    // Extraction APIs
    managerkit::registerExtractionApis();
    // Bookmark APIs
    managerkit::registerBookmarkApis();
    // Control APIs
    managerkit::registerControlApis();
    // Motion detection APIs
    managerkit::registerMotionDetectionApis();
    // Sync APIs
    managerkit::registerSyncDbApis();
    // Monitor APIs
    managerkit::registerMonitorApis();
    // Storage tiering APIs
    GET_CONFIG(bool, legacy_record_cleanup_enabled, Storage::kLegacyRecordCleanupEnabled);
    if (!legacy_record_cleanup_enabled) {
        managerkit::registerStorageApis();
    }
    // Historical sd card sync APIs
    managerkit::registerHistoricalSDCardSyncApis();
}

void unInstallWebApi(){
    s_player_proxy.clear();
    s_ffmpeg_src.clear();
    s_pusher_proxy.clear();
#if defined(ENABLE_RTPPROXY)
    s_rtp_server.clear();
#endif
#if defined(ENABLE_VIDEOSTACK) && defined(ENABLE_FFMPEG) && defined(ENABLE_X264)
    VideoStackManager::Instance().clear();
#endif

    managerkit::unregisterExtractionApis();
    managerkit::unregisterControlApis();

    NoticeCenter::Instance().delListener(&web_api_tag);
}
