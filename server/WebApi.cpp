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

#include "Local/SearchEngine.h"
#include "Storage/Bookmark.h"
#include "Storage/UserEntity.h"
#include "Storage/Certification.h"
#include "Server/GlobalMonitor.h"
#include "Manager.h"
#include "Camera/GenericRtspCameraImp.h"
#include "Onvif/Onvif.h"
#include "Onvif/SoapUtil.h"
#include "WebApiErrCode.h"
#include "Common/StrUtil.h"
#include "Control/SubnetScan.h"

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

static bool checkUserAuthor(const string &resource_id, const string &jwt_token) {
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

// Push stream proxy list
static ServiceController<PusherProxy> s_pusher_proxy;

// FFmpeg pull stream proxy list
static ServiceController<FFmpegSource> s_ffmpeg_src;

// FFmpeg extractor proxy list
static ServiceController<FFmpegExtractor> s_ffmpeg_extractor;

// Subnet scan proxy list
static ServiceController<SubnetScan> s_subnet_scan;

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
        FFmpegSnap::makeSnap(allArgs["async"], allArgs["url"], new_snap_tmp, allArgs["timeout_sec"], [invoker, allArgs, new_snap, new_snap_tmp](bool success, const string &err_msg) {
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
    static auto findDeviceSource = [](const string &device_id) {
        DeviceTuple tuple;
        tuple.vhost = DEFAULT_VHOST;
        tuple.device_id = device_id;
        return DeviceSource::find(tuple.vhost, tuple.device_id);
    };

    api_regist("/media/esc/recordedTimePeriod", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("cameraId", "startTime", "endTime", "periodType", "detail");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string camera_id = allArgs["cameraId"];
            uint64_t start_time = allArgs["startTime"];
            uint64_t end_time = allArgs["endTime"];
            int period_type = allArgs["periodType"];
            int detail = allArgs["detail"];
            bool include_motion = allArgs["motion"];

            if (!start_time) {
                start_time = time(nullptr) - 24 * 3600;
            }

            if (!end_time) {
                end_time = time(nullptr);
            }

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, "", "" };
            SearchEngine::findTimePeriod(tuple, start_time, end_time, period_type, detail, include_motion, [&](const SockException &ex, const Value &data) {
                if (ex) {
                    RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                } else {
                    val["data"] = data;
                    InfoL << "Get recorded time period success";
                    invoker(200, headerOut, val.toStyledString());
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });

    // Get screenshot cache or real-time screenshot
    api_regist("/media/esc/recordedThumnail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("cameraId", "streamId", "pos");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string camera_id = allArgs["cameraId"];
            string stream_id = allArgs["streamId"];
            string pos_str = allArgs["pos"];

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
            TimeQuery::Ptr query;
            try {
                query = std::make_shared<TimeQuery>(tuple);
            } catch(...) {}

            string src_path;
            uint64_t pos_time = 0;
            if (query) {
                if (pos_str == "latest") {
                    // todo: get latest jpeg record
                    auto ret = findDeviceSource(tuple.app);
                    if (ret) {
                        auto ptr = dynamic_pointer_cast<GenericRtspCameraImp>(ret);
                        if (ptr) {
                            auto stats_imp = ptr->getCameraStatisticImp();
                            if (stats_imp) {
                                auto params = stats_imp->getParams();
                                if (params.storage_map.find(tuple.stream) != params.storage_map.end()) {
                                    auto last_archived_time = params.storage_map[tuple.stream].archiveEndTime;
                                    if (last_archived_time > 0) {
                                        auto block = query->getLastBlock(last_archived_time);
                                        if (block) {
                                            pos_time = block->start_time();
                                            src_path = decodeBase64(block->file_path());
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    pos_time = stoll(pos_str);
                    auto start_time = pos_time - 60;
                    auto end_time = pos_time + 60;
                    query->getRecordedTimePeriod(start_time, end_time, [&pos_time, &src_path](const vector<TimeBlock> &blocks) {
                        for (const auto &block : blocks) {
                            if (block.start_time() > pos_time) {
                                break;
                            }
                            pos_time = block.start_time();
                            src_path = decodeBase64(block.file_path());
                        }
                    });
                }
            }

            if (src_path.empty() || pos_time == 0) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_TIMELINE_NOT_FOUND, "No data in period");
                return;
            }

            GET_CONFIG(string, snap_root, API::kSnapRoot);
            int expire_sec = 60;

            bool have_old_snap = false, res_old_snap = false;
            auto path = camera_id + "/" + stream_id;
            auto scan_path = File::absolutePath(path, snap_root) + "/";
            string new_snap = StrPrinter << scan_path << pos_time << ".jpeg";

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
                // At the same time, prevent the FFmpeg process from being started multiple times by continuously trying to call this API during the FFmpeg screenshot generation process
                auto file = File::create_file(new_snap, "wb");
                if (file) {
                    fclose(file);
                }
            }

            // Start the FFmpeg process, start taking screenshots, generate temporary files, replace them with formal files after successful screenshots
            auto new_snap_tmp = new_snap + ".tmp";
            FFmpegSnap::makeSnap(false, src_path, new_snap_tmp, 2, [invoker, allArgs, new_snap, new_snap_tmp](bool success, const string &err_msg) {
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
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });

    static auto addFFmpegExtractor = [](MediaTuple &tuple, ExtractOptions &options, const function<void(const SockException &ex, const string &key)> &cb) {
        auto full_key = tuple.shortUrl() + "/" + to_string(options.start_time) + "/" + to_string(options.end_time) + "/" + options.filename;
        auto key = MD5(full_key).hexdigest();
        if (s_ffmpeg_extractor.find(key)) {
            // Already create
            cb(SockException(Err_success), key);
            return;
        }

        auto ffmpeg = s_ffmpeg_extractor.make(key, tuple, options);

        ffmpeg->setOnClose([key]() { s_ffmpeg_extractor.erase(key); });

        GET_CONFIG(string, extract_path, API::kExtractRoot)
        ffmpeg->makeExtract(key, extract_path, [cb, key](const SockException &ex) {
            if (ex) {
                s_ffmpeg_src.erase(key);
            }
            cb(ex, key);
        });
    };
    api_regist("/media/esc/extractArchived/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("cameraId", "streamId", "startTime", "endTime", "filename");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            auto camera_id = allArgs["cameraId"];
            auto stream_id = allArgs["streamId"];
            auto start_time = allArgs["startTime"];
            auto end_time = allArgs["endTime"];
            auto filename = allArgs["filename"];
            auto description = allArgs["description"];
            auto user_id = allArgs["_user_id"];
            auto user_name = allArgs["_user_name"];
            auto jwt_token = allArgs["_jwt_token"];

            if (!findDeviceSource(camera_id)) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            if (!end_with(filename, ".mp4") && !end_with(filename, ".mkv") && !end_with(filename, ".avi")) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_EXTENSION, "Only support file extension: .mp4, .mkv, .avi");
                return;
            }

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
            ExtractOptions options = { start_time, end_time, filename, description, user_id, user_name };

            addFFmpegExtractor(tuple, options, [invoker, val, headerOut, jwt_token](const SockException &ex, const string &key) mutable {
                if (ex) {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_FAILED, ex.what());
                } else {
                    UserAuthorManager::Instance().addAuthorCache(key, jwt_token, true, 600);
                    val["data"]["key"] = key;
                    invoker(201, headerOut, val.toStyledString());
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });

    api_regist("/media/esc/extractArchived/progress", [](API_ARGS_MAP_ASYNC) {
        CHECK_USER_AUTHOR("key");

        auto ffmpeg = s_ffmpeg_extractor.find(allArgs["key"]);
        if (!ffmpeg) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_KEY_NOT_FOUND, "Key not found");
            return;
        }

        if (ffmpeg->finished() && !ffmpeg->success()) {
            auto err_detail = "Extract video failed: " + ffmpeg->errMsg();
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_FAILED, err_detail.data());
            return;
        }

        val["data"]["progress"] = ffmpeg->progress();
        val["data"]["ready"] = ffmpeg->finished() && ffmpeg->success() ? true : false;
        invoker(202, headerOut, val.toStyledString());
    });

    api_regist("/media/esc/extractArchived/download", [](API_ARGS_MAP_ASYNC) {
        CHECK_USER_AUTHOR("key");

        auto key = allArgs["key"];
        auto ffmpeg = s_ffmpeg_extractor.find(allArgs["key"]);
        if (!ffmpeg || !ffmpeg->finished() || !ffmpeg->success()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_KEY_NOT_FOUND, "Key not found");
            return;
        }

        StrCaseMap res_header;
        auto save_name = ffmpeg->getFilename();
        auto save_path = ffmpeg->getSavePath();
        if (!save_name.empty()) {
            res_header.emplace("Content-Disposition", "attachment;filename=\"" + save_name + "\"");
        }
        invoker.responseFile(allArgs.parser.getHeader(), res_header, save_path);
    });

    api_regist("/media/esc/extractArchived/delete", [](API_ARGS_MAP) {
        CHECK_USER_AUTHOR("key");

        val["data"]["flag"] = s_ffmpeg_extractor.erase(allArgs["key"]) == 1;
    });

    api_regist("/media/esc/extractArchived/list", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        auto jwt_token = allArgs["_jwt_token"];

        s_ffmpeg_extractor.for_each([&](const std::string &key, const FFmpegExtractor::Ptr &src) {
            if (!checkUserAuthor(key, jwt_token)) {
                return;
            }
            Json::Value item;
            item["key"] = key;
            item["progress"] = src->progress();
            item["ready"] = src->finished() && src->success() ? true : false;
            val["data"].append(item);
        });
    });

    api_regist("/media/esc/bookmark/search", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("start_time", "end_time", "page", "size", "sort");

        string camera_id = allArgs["camera_id"];
        int64_t start_time = allArgs["start_time"];
        int64_t end_time = allArgs["end_time"];
        string search = allArgs["search"];
        int page = allArgs["page"];
        int size = allArgs["size"];
        string sort = allArgs["sort"];
        string user_id = allArgs["_user_id"];

        if (size <= 0) size = 1;

        auto imp = std::make_shared<BookmarkImp>();
        auto ret = imp->search(start_time, end_time, camera_id, user_id, search, page, size, sort);
        auto user_imp = std::make_shared<UserEntityImp>();

        val["data"] = arrayValue;
        for (const Bookmark &b : ret) {
            Value b_json;
            b_json["id"] = b.guid;
            b_json["camera_id"] = b.camera_guid;
            b_json["start_time"] = b.start_time;
            b_json["duration"] = b.duration;
            b_json["name"] = b.name ? b.name.value() : "";
            b_json["end_time"] = b.end_time ? b.end_time.value() : -1;
            b_json["description"] = b.description ? b.description.value() : "";
            b_json["creator_guid"] = b.creator_guid ? b.creator_guid.value() : "";
            string username;
            if (b.creator_guid) {
                auto users = user_imp->findById(b.creator_guid.value());
                if (users.size() > 0) {
                    username = users[0].userName ? users[0].userName.value() : "";
                }
            }
            b_json["creator"] = username;
            b_json["created"] = b.created ? b.created.value() : -1;
            auto tags = imp->findTagsByBookmark(b.guid);
            b_json["tags"] = tags;
            val["data"].append(b_json);
        }

        auto total = imp->count(start_time, end_time, camera_id, user_id, search);
        val["currentPage"] = page;
        val["totalItems"] = total;
        val["totalPages"] = static_cast<int>(std::ceil(static_cast<double>(total) / size));
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/esc/bookmark/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("name", "camera_id", "start_time", "duration");

        auto on_access = [allArgs, &val, &invoker, &headerOut]() {
            string name = allArgs["name"];
            string description = allArgs["description"];
            string camera_id = allArgs["camera_id"];
            int64_t start_time = allArgs["start_time"];
            int64_t end_time = allArgs["end_time"];
            int64_t duration = allArgs["duration"];
            string tags = allArgs["tags"];

            auto ret = findDeviceSource(camera_id);
            if (!ret) {
                val["data"]["flag"] = false;
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            Bookmark bm;
            bm.name = name;
            bm.description = description;
            bm.camera_guid = camera_id;
            bm.start_time = start_time;
            bm.end_time = end_time;
            bm.duration = duration;
            bm.creator_guid = allArgs["_user_id"];
            bm.created = time(nullptr);

            auto imp = std::make_shared<BookmarkImp>();
            imp->add(bm, tags);

            val["data"]["flag"] = true;
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["camera_id"], on_access);
    });

    api_regist("/media/esc/bookmark/update", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("id", "camera_id", "start_time", "duration");

        auto on_access = [allArgs, &val, &invoker, &headerOut]() {
            string id = allArgs["id"];
            string name = allArgs["name"];
            string description = allArgs["description"];
            string camera_id = allArgs["camera_id"];
            int64_t start_time = allArgs["start_time"];
            int64_t end_time = allArgs["end_time"];
            int64_t duration = allArgs["duration"];
            string tags = allArgs["tags"];

            auto imp = std::make_shared<BookmarkImp>();
            auto ret = imp->findById(id);
            if (!ret.size()) {
                WarnL << "Bookmark " << id << " not found";
                val["data"]["flag"] = false;
                RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_NOT_FOUND, "Bookmark not found");
                return;
            }

            Bookmark bm = ret[0];
            bm.name = name;
            bm.description = description;
            bm.camera_guid = camera_id;
            bm.start_time = start_time;
            bm.end_time = end_time;
            bm.duration = duration;
            bm.creator_guid = allArgs["_user_id"];
            bm.created = time(nullptr);

            imp->update(bm, tags);
            val["data"]["flag"] = true;
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["camera_id"], on_access);
    });

    api_regist("/media/esc/bookmark/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("id");

        auto id = allArgs["id"];

        auto imp = std::make_shared<BookmarkImp>();
        auto ret = imp->findById(id);
        if (!ret.size()) {
            WarnL << "Bookmark " << id << " not found";
            val["data"]["flag"] = false;
            RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_NOT_FOUND, "Bookmark not found");
            return;
        }

        auto on_access = [&val, &invoker, &headerOut, imp, id]() {
            imp->remove(id);
            val["data"]["flag"] = true;
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(ret[0].camera_guid, on_access);
    });

    api_regist("/media/esc/bookmark/mostUsedTags", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("size");

        int size = allArgs["size"];
        if (size < 0) size = 1;
        
        auto imp = std::make_shared<BookmarkTagCountImp>();
        auto tags = imp->findTagsByCountDesc(size);

        val["data"] = arrayValue;
        for (const auto &tag : tags) {
            val["data"].append(tag);
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/esc/bookmark/recent", [](API_ARGS_MAP_ASYNC) { 
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("size", "sort"); 

        string camera_id = allArgs["camera_id"];
        int size = allArgs["size"];
        string sort = allArgs["sort"];
        string user_id = allArgs["_user_id"];
        if (size < 0) size = 1;
        if (size > 50) size = 50;
        
        auto imp = std::make_shared<BookmarkImp>();
        auto ret =  imp->findRecentById(camera_id, user_id, size, sort);
        auto user_imp = std::make_shared<UserEntityImp>();

        val["data"] = arrayValue;
        for (const Bookmark &b : ret) {
            Value b_json;
            b_json["id"] = b.guid;
            b_json["camera_id"] = b.camera_guid;
            b_json["start_time"] = b.start_time;
            b_json["duration"] = b.duration;
            b_json["name"] = b.name ? b.name.value() : "";
            b_json["end_time"] = b.end_time ? b.end_time.value() : -1;
            b_json["description"] = b.description ? b.description.value() : "";
            b_json["creator_guid"] = b.creator_guid ? b.creator_guid.value() : "";
            string username;
            if (b.creator_guid) {
                auto users = user_imp->findById(b.creator_guid.value());
                if (users.size() > 0) {
                    username = users[0].userName ? users[0].userName.value() : "";
                }
            }
            b_json["creator"] = username;
            b_json["created"] = b.created ? b.created.value() : -1;
            auto tags = imp->findTagsByBookmark(b.guid);
            b_json["tags"] = tags;
            val["data"].append(b_json);
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/description", [](API_ARGS_MAP) {
        Value info;
        info["mediaServerId"] = mINI::Instance()[General::kMediaServerId];
        info["version"] = kServerName;
        auto osinfo = GlobalMonitor::Instance().getOsInfo();
        info["osInfo"]["platform"] = osinfo.platform;
        info["osInfo"]["variant"] = osinfo.variant;
        info["osInfo"]["variantVerison"] = osinfo.variant_version;
        info["httpPort"] =  static_cast<int>(mINI::Instance()["http.port"]);
        info["httpsPort"] = static_cast<int>(mINI::Instance()["http.sslport"]);
        info["clientUseSsl"] = false;
        info["maxDevice"] =  estimateMaxAvailableDevice();
        val["data"] = info;
    });

    api_regist("/media/mserver/register", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("mediaServerId", "domain", "ip", "httpPort", "httpsPort", "preferSSL")

        string mediaServerId_ = allArgs["mediaServerId"];
        string apiDomain = allArgs["domain"];
        string apiIp = allArgs["ip"];
        int httpPort = allArgs["httpPort"];
        int httpsPort = allArgs["httpsPort"];
        bool preferSSL = allArgs["preferSSL"];
        string mediaServerDomain = allArgs["mediaServerDomain"];
        string mediaServerCert = allArgs["mediaServerCert"];

        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (mediaServerId != mediaServerId_) {
            val["data"]["changed"] = 0;
            invoker(200, headerOut, val.toStyledString());
            return;
        }

        auto origin_urls = UriUtils::getUriList(apiDomain, apiIp, httpPort, httpsPort, preferSSL);

        Broadcast::HealthInvoker on_health_check = [allArgs, origin_urls, mediaServerDomain, mediaServerCert, val, headerOut, invoker](const string& err, const int& idx) mutable {
            if (!err.empty()) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_HEALTH_CHECK_API_FAILED, err.data());
                return;
            }

            auto apiUrlTmp = origin_urls[idx];
            int changed = 0;
            bool need_to_restart = false;
            auto &ini = mINI::Instance();

            // get new config and compare to old one of system
            if (ini[Hook::kApiUrl] != apiUrlTmp) {
                ini[Hook::kApiUrl] = apiUrlTmp;
                ++changed;
                need_to_restart = true;
            }

            if (!mediaServerDomain.empty() && !mediaServerCert.empty()) {
                if (ini[Manager::kMediaServerDomain] != mediaServerDomain) {
                    ini[Manager::kMediaServerDomain] = mediaServerDomain;
                    ++changed;
                }
                
                {
                    auto imp = std::make_shared<CertificateImp>();
                    if (!imp->certExist(mediaServerDomain, mediaServerCert)) {
                        imp->saveCert(mediaServerDomain, mediaServerCert);
                        ++changed;
                    }
                }
            }

            if (changed > 0) {
                // notify to reload config and dump ini file
                NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
                ini.dumpFile(g_ini_file);
            }

            if (need_to_restart) {
                // notify to restart server
                NOTICE_EMIT(BroadcastSystemAuditLogArgs, Broadcast::kBroadcastSystemAuditLog, SystemAuditLogType::MEDIA_SERVER_SHUTTING_DOWN_CONFIG, string("Restart due to updating api configuration"));
                NOTICE_EMIT(BroadcastRestartServerArgs, Broadcast::kBroadcastRestartServer);
            }

            val["data"]["changed"] = changed;
            invoker(200, headerOut, val.toStyledString());
        };
        
        auto flag = NOTICE_EMIT(BroadcastHealthCheckServiceArgs, Broadcast::kBroadcastHealthCheckService, origin_urls, on_health_check);
        if (!flag) {
            // Nobody to handle health check service, just return failed
            on_health_check("No handler to handle health check api service", -1);
        }
    });

    api_regist("/media/mserver/systemStatistic", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        CHECK_READ_MSERVER_PERMISSION();
        // string id = allArgs["mediaServerId"];
        // GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        // if (id != mediaServerId) {
        //     RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
        //     return;
        // }
        val["data"] = makeSystemStatisticJson();
    });

    api_regist("/media/mserver/device/discovery", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_ADD_CAMERA_PERMISSION();
        CHECK_ARGS_("address","port", "defaultPort");

        string address = allArgs["address"];
        int port = allArgs["port"];
        bool defaultPort = allArgs["defaultPort"];
        string username = allArgs["username"];
        string password = allArgs["password"];

        SubnetScan::discovery_device(address, port, defaultPort, username, password, [=](const SockException &ex, const DeviceScanResult &data) mutable {
            if (ex) {
                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                return;
            }
            val["data"] = toJsonValue(data);
            invoker(200, headerOut, val.toStyledString());
        });
    });

    api_regist("/media/mserver/device/subnetScan", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_ADD_CAMERA_PERMISSION();
        CHECK_ARGS_("startIp", "endIp", "port", "defaultPort");

        string startIp = allArgs["startIp"];
        string endIp = allArgs["endIp"];
        int port = allArgs["port"];
        bool defaultPort = allArgs["defaultPort"];
        string username = allArgs["username"];
        string password = allArgs["password"];

        if (!SockUtil::is_ipv4(startIp.data()) || !SockUtil::is_ipv4(endIp.data())) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_IP, "startIp or endIp must be a IPv4");
            return;
        }

        val["data"] = arrayValue;
        auto ip_range = SockUtil::get_ipv4_range(startIp, endIp);
        for (auto &ip : ip_range) {
            SubnetScan::discovery_device(ip, port, defaultPort, username, password, [&](const SockException &ex, const DeviceScanResult &data) {
                if (!ex) {
                    val["data"].append(toJsonValue(data));
                }
            });
        }
        invoker(200, headerOut, val.toStyledString());
    });

    static auto addSubnetScan = [](SubnetScanOption &option, const function<void(const SockException &ex, const string &key)> &cb) {
        string full_key = (StrPrinter << option.startIp << "/" << option.endIp << "/" << option.port << "/" << option.username << "/" << option.password);
        string key = MD5(full_key).hexdigest();
        if (s_subnet_scan.find(key)){
            // Already create
            cb(SockException(Err_success), key);
            return;
        }
        auto scanner = s_subnet_scan.make(key, option);
        scanner->setOnClose([key]() { s_subnet_scan.erase(key); });

        scanner->makeScan(key, [cb, key](const SockException &ex) {
            if (ex) {
                s_subnet_scan.erase(key);
            }
            cb(ex, key);
        });
    };

    api_regist("/media/mserver/device/subnetScan/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_ADD_CAMERA_PERMISSION();
        CHECK_ARGS_("startIp", "endIp", "port", "defaultPort");

        string startIp = allArgs["startIp"];
        string endIp = allArgs["endIp"];
        int port = allArgs["port"];
        bool defaultPort = allArgs["defaultPort"];
        string username = allArgs["username"];
        string password = allArgs["password"];
        string jwt_token = allArgs["_jwt_token"];
        
        if (!SockUtil::is_ipv4(startIp.data()) || !SockUtil::is_ipv4(endIp.data())) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_IP, "startIp or endIp must be a IPv4");
            return;
        }

        SubnetScanOption option;
        option.startIp = startIp;
        option.endIp = endIp;
        option.port = port;
        option.defaultPort = defaultPort;
        option.username = username;
        option.password = password;

        addSubnetScan(option, [invoker, val, headerOut, jwt_token](const SockException &ex, const string &key) mutable {
            if (ex) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_SUBNETSCAN_FAILED, ex.what());
            } else {
                UserAuthorManager::Instance().addAuthorCache(key, jwt_token, true, 600);
                val["data"]["key"] = key;
                invoker(201, headerOut, val.toStyledString());
            }
        });
    });

    api_regist("/media/mserver/device/subnetScan/progress", [](API_ARGS_MAP_ASYNC) {
        CHECK_USER_AUTHOR("key");

        std::string key = allArgs["key"];
        auto scanner = s_subnet_scan.find(key);
        if (!scanner) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_SCAN_KEY_NOT_FOUND, "Scan key not found");
            return;
        }

        bool isFinished = scanner->finished();
        double  progress = scanner->progress();
        auto devices = scanner->result();
        Json::Value ret = Json::arrayValue;
        for (auto &d : devices) {
            ret.append(toJsonValue(d));
        }

        val["data"]["finished"] = isFinished;
        val["data"]["progress"] = progress;
        val["data"]["devices"] = ret;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/device/subnetScan/delete", [](API_ARGS_MAP) {
        CHECK_USER_AUTHOR("key");

        val["data"]["flag"] = s_subnet_scan.erase(allArgs["key"]) == 1;
    });

    api_regist("/media/mserver/device/ptz_control", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PTZ_CONTROL_PERMISSION();
        CHECK_ARGS_("deviceId", "direct", "speed");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string strDirect = allArgs["direct"];
            int speed = allArgs["speed"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            auto ownership = ret->getOwnership();
            if (!ownership) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OWNERSHIP_BY_OTHER, "Device is controlled by other user");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        impl->PTZMove(strDirect, speed, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    /* Unreachable */
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/goto_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PTZ_CONTROL_PERMISSION();
        CHECK_ARGS_("deviceId", "presetToken", "isUserPreset");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            bool isUserPreset = allArgs["isUserPreset"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            auto ownership = ret->getOwnership();
            if (!ownership) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OWNERSHIP_BY_OTHER, "Device is controlled by other user");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        impl->PTZGotoPreset(presetToken, isUserPreset, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    /* Unreachable */
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/get_presets", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PTZ_CONTROL_PERMISSION();
        CHECK_ARGS_("deviceId");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            val["data"] = makeDevicePTZPresetJson(ret);
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/set_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PTZ_CONTROL_PERMISSION();
        CHECK_ARGS_("deviceId", "presetToken", "presetName");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            string presetName = allArgs["presetName"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        impl->addUserPTZPreset(presetToken, presetName, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    /* Unreachable */
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/remove_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PTZ_CONTROL_PERMISSION();
        CHECK_ARGS_("deviceId", "presetToken", "presetName");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            string presetName = allArgs["presetName"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        impl->removeUserPTZPreset(presetToken, presetName, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    /* Unreachable */
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/storage/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_READ_MSERVER_PERMISSION();
        CHECK_ARGS_("mediaServerId");

        string id = allArgs["mediaServerId"];
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (id != mediaServerId) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }
        val["data"] = makeSystemStorageJson();
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/device/storage", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_READ_MSERVER_PERMISSION();
        CHECK_ARGS_("mediaServerId");

        string id = allArgs["mediaServerId"];
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (id != mediaServerId) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }
        val["data"] = makeStorageStatisticJson();
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/device/statistic", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_READ_MSERVER_PERMISSION();
        CHECK_ARGS_("mediaServerId");

        string id = allArgs["mediaServerId"];
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (id != mediaServerId) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }

        getServerStatisticJson([=](Json::Value &data) mutable {
            val["data"] = data;
            invoker(200, headerOut, val.toStyledString());
        });
    });

    static auto findPlaybackStream = [](MediaSource::Ptr &ret, const string &url_in) {
        MediaInfo info_in;
        info_in.parse(url_in);
        string url = StrPrinter << "/" << info_in.app << "/" << info_in.stream;

        string url_prefix = "/media";
        string ts_suffix = ".live.ts";
        string flv_suffix = ".live.flv";
        string fmp4_suffix = ".live.mp4"; 
        auto prefix_size = url_prefix.size();
        if (prefix_size > 0) {
            if (url.size() < prefix_size || strncasecmp(url.data(), url_prefix.data(), prefix_size)) {
                // Prefix not found
                return false;
            }
            // Remove special prefix from url
            url.erase(0, prefix_size);
        }
        string schema;
        if (end_with(url, fmp4_suffix)) {
            schema = FMP4_SCHEMA;
            url.erase(url.size() - fmp4_suffix.size());
        } else if (end_with(url, ts_suffix)) {
            schema = TS_SCHEMA;
            url.erase(url.size() - ts_suffix.size());
        } else if (end_with(url, flv_suffix)) {
            schema = RTMP_SCHEMA;
            url.erase(url.size() - flv_suffix.size());
        } else {
            // Suffix not found
            return false;
        }

        MediaInfo media_info(schema + "://" + DEFAULT_VHOST + url);
        if (media_info.app.empty() || media_info.stream.empty()) {
            // URL is invalid
            return false;
        }

        ret = MediaSource::find(media_info.schema, media_info.vhost, media_info.app, media_info.stream);
        return ret != nullptr;
    };

    api_regist("/media/mserver/playback/speed", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("url", "speed");
        string url = allArgs["url"];

        MediaSource::Ptr src;
        if (!findPlaybackStream(src, url)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STREAM_NOT_FOUND, "Playback stream not found");
            return;
        }
        auto tuple = src->getMediaTuple();
        auto stream = tuple.stream;
        string device_id = split(stream, "/").front();

        auto on_access = [allArgs, val, invoker, headerOut, src]() mutable {
            auto speed = allArgs["speed"].as<float>();
            src->getOwnerPoller()->async([=]() mutable {
                bool flag = src->speed(speed);
                val["code"] = flag ? ApiErrCode::CODE_SUCCESS : ApiErrCode::CODE_OTHER_EXCEPTION;
                val["msg"] = flag ? "Success" : "Failed";
                val["result"] = flag ? 0 : -1;                                                                                                                                                                                                                                                                                                
                invoker(200, headerOut, val.toStyledString());
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(device_id, on_access);
    });

    api_regist("/media/mserver/healthcheck", [](API_ARGS_MAP) {
        val["data"]["mediaServerId"] = mINI::Instance()[General::kMediaServerId];
    });

    api_regist("/media/mserver/incur", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("mediaServerId");

        string id = allArgs["mediaServerId"];
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (id != mediaServerId) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }

        NOTICE_EMIT(BroadcastReloadApiConfigArgs, Broadcast::kBroadcastReloadApiConfig);
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/esc/searchMotion", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_PLAYBACK_PERMISSION();
        CHECK_ARGS_("cameraId", "startTime", "endTime", "roiMask");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string camera_id = allArgs["cameraId"];
            uint64_t start_time = allArgs["startTime"];
            uint64_t end_time = allArgs["endTime"];
            string roi_mask = allArgs["roiMask"];

            if (!start_time) {
                start_time = time(nullptr) - 24 * 3600;
            }

            if (!end_time) {
                end_time = time(nullptr);
            }

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, "", "" };
            SearchEngine::findMotionPeriodByRoi(tuple, start_time, end_time, roi_mask, [&](const SockException &ex, const Value &data) {
                if (ex) {
                    RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                } else {
                    val["data"] = data;
                    InfoL << "Search motion time by ROI success";
                    invoker(200, headerOut, val.toStyledString());
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });

    api_regist("/media/mserver/device/statistics", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("id");

        string id = allArgs["id"];
        auto device = findDeviceSource(id);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
            return;
        }

        val["data"] = makeDeviceStatisticJson(device);
        invoker(200, headerOut, val.toStyledString());
    });
}

void unInstallWebApi(){
    s_player_proxy.clear();
    s_ffmpeg_src.clear();
    s_ffmpeg_extractor.clear();
    s_subnet_scan.clear();
    s_pusher_proxy.clear();
#if defined(ENABLE_RTPPROXY)
    s_rtp_server.clear();
#endif
#if defined(ENABLE_VIDEOSTACK) && defined(ENABLE_FFMPEG) && defined(ENABLE_X264)
    VideoStackManager::Instance().clear();
#endif

    NoticeCenter::Instance().delListener(&web_api_tag);
}
