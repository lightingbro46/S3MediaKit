#ifndef S3MEDIAKIT_WEBAPI_H
#define S3MEDIAKIT_WEBAPI_H

#include <string>
#include <functional>
#include "json/json.h"
#include "Common/Parser.h"
#include "Network/Socket.h"
#include "Http/HttpSession.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Player/PlayerProxy.h"
#include "FFmpegSource.h"

#if defined(ENABLE_WEBRTC)
#include "webrtc/WebRtcTransport.h"
#endif

// Configuration file path
extern std::string g_ini_file;

namespace mediakit {
// //////////RTSP server configuration///////////
namespace Rtsp {
extern const std::string kPort;
} //namespace Rtsp

// //////////RTMP server configuration///////////
namespace Rtmp {
extern const std::string kPort;
} //namespace RTMP
}  // namespace mediakit

namespace API {
typedef enum {
    NotFound = -500,//Not found
    Exception = -400,//Code throws exception
    InvalidArgs = -300,//Illegal parameters
    SqlFailed = -200,//SQL execution failed
    AuthFailed = -100,//Authentication failed
    OtherFailed = -1,//Business code execution failed,
    Success = 0//Execution successfully
} ApiErr;

extern const std::string kSecret;
extern const std::string kSnapRoot;
extern const std::string kExtractRoot;
}//namespace API

class ApiRetException: public std::runtime_error {
public:
    ApiRetException(const char *str = "success" , int code = API::Success, int status_code = 200) : runtime_error(str) {
        _code = code;
        _status_code = status_code;
    }
    int code() { return _code; }
    int status_code() { return _status_code; }
private:
    int _code;
    int _status_code;
};

class AuthException : public ApiRetException {
public:
    AuthException(const char *str, int code = API::AuthFailed) : ApiRetException(str, code, 401) {}
};

class InvalidArgsException: public ApiRetException {
public:
    InvalidArgsException(const char *str, int code = API::InvalidArgs) : ApiRetException(str, code, 400) {}
};

class SuccessException: public ApiRetException {
public:
    SuccessException(int code = API::Success) : ApiRetException("success", code, 200) {}
};

using ApiArgsType = std::map<std::string, std::string, mediakit::StrCaseCompare>;

template<typename Args, typename Key>
std::string getValue(Args &args, const Key &key) {
    auto it = args.find(key);
    if (it == args.end()) {
        return "";
    }
    return it->second;
}

template<typename Key>
std::string getValue(Json::Value &args, const Key &key) {
    auto value = args.find(key);
    if (value == nullptr) {
        return "";
    }
    return value->asString();
}

template<typename Key>
std::string getValue(std::string &args, const Key &key) {
    return "";
}

template <typename Key>
std::string getValue(const mediakit::Parser &parser, const Key &key) {
    auto ret = getValue(parser.getUrlArgs(), key);
    if (!ret.empty()) {
        return ret;
    }
    return getValue(parser.getHeader(), key);
}

template<typename Key>
std::string getValue(mediakit::Parser &parser, const Key &key) {
    return getValue((const mediakit::Parser &) parser, key);
}

template<typename Args, typename Key>
std::string getValue(const mediakit::Parser &parser, Args &args, const Key &key) {
    auto ret = getValue(args, key);
    if (!ret.empty()) {
        return ret;
    }
    return getValue(parser, key);
}

template<typename Args>
class HttpAllArgs {
    mediakit::Parser* _parser = nullptr;
    Args* _args = nullptr;
public:
    const mediakit::Parser& parser;
    Args& args;

    HttpAllArgs(const mediakit::Parser &p, Args &a): parser(p), args(a) {}

    HttpAllArgs(const HttpAllArgs &that): _parser(new mediakit::Parser(that.parser)),
                                          _args(new Args(that.args)),
                                          parser(*_parser), args(*_args) {}
    ~HttpAllArgs() {
        if (_parser) {
            delete _parser;
        }
        if (_args) {
            delete _args;
        }
    }

    template<typename Key>
    toolkit::variant operator[](const Key &key) const {
        return (toolkit::variant)getValue(parser, args, key);
    }

    const Args& getArgs() const {
        return args;
    }

    const mediakit::Parser &getParser() const {
        return parser;
    }
};

using ArgsMap = HttpAllArgs<ApiArgsType>;
using ArgsJson = HttpAllArgs<Json::Value>;
using ArgsString = HttpAllArgs<std::string>;

#define API_ARGS_MAP toolkit::SockInfo &sender, mediakit::HttpSession::KeyValue &headerOut, const ArgsMap &allArgs, Json::Value &val
#define API_ARGS_MAP_ASYNC API_ARGS_MAP, const mediakit::HttpSession::HttpResponseInvoker &invoker
#define API_ARGS_JSON toolkit::SockInfo &sender, mediakit::HttpSession::KeyValue &headerOut, const ArgsJson &allArgs, Json::Value &val
#define API_ARGS_JSON_ASYNC API_ARGS_JSON, const mediakit::HttpSession::HttpResponseInvoker &invoker
#define API_ARGS_STRING toolkit::SockInfo &sender, mediakit::HttpSession::KeyValue &headerOut, const ArgsString &allArgs, Json::Value &val
#define API_ARGS_STRING_ASYNC API_ARGS_STRING, const mediakit::HttpSession::HttpResponseInvoker &invoker
#define API_ARGS_VALUE sender, headerOut, allArgs, val

// Register http request parameters as map<string, variant, StrCaseCompare> type http api
void api_regist(const std::string &api_path, const std::function<void(API_ARGS_MAP)> &func);
// Register http request parameters as map<string, variant, StrCaseCompare> type, but can be replied asynchronously http api
void api_regist(const std::string &api_path, const std::function<void(API_ARGS_MAP_ASYNC)> &func);

// Register http request parameters as Json::Value type http api (can support multi-level nested json parameter objects)
void api_regist(const std::string &api_path, const std::function<void(API_ARGS_JSON)> &func);
// Register http request parameters as Json::Value type, but can be replied asynchronously http api
void api_regist(const std::string &api_path, const std::function<void(API_ARGS_JSON_ASYNC)> &func);

// Register http request parameters as http original request information http api
void api_regist(const std::string &api_path, const std::function<void(API_ARGS_STRING)> &func);
// Register http request parameters as http original request information asynchronous reply http api
void api_regist(const std::string &api_path, const std::function<void(API_ARGS_STRING_ASYNC)> &func);

template<typename Args, typename Key>
bool checkArgs(Args &args, const Key &key) {
    return !args[key].empty();
}

template<typename Args, typename Key, typename ...KeyTypes>
bool checkArgs(Args &args, const Key &key, const KeyTypes &...keys) {
    return checkArgs(args, key) && checkArgs(args, keys...);
}

// Check whether the http url, body or http header parameters are empty
#define CHECK_ARGS(...)  \
    if(!checkArgs(allArgs,##__VA_ARGS__)){ \
        throw InvalidArgsException("Required parameter missed: " #__VA_ARGS__); \
    }

#define CHECK_ARGS_(...)  \
    if(!checkArgs(allArgs,##__VA_ARGS__)){ \
        throw InvalidArgsException("Required parameter missed: " #__VA_ARGS__, ApiErrCode::CODE_INVALID_ARGS); \
    }

// Check whether the http parameters contain the secret key, the ip of 127.0.0.1 does not check the key
// Check whether it is in the ip whitelist at the same time
#define CHECK_SECRET() \
    do { \
        auto ip = sender.get_peer_ip(); \
        if (!HttpFileManager::isIPAllowed(ip)) { \
            throw AuthException("Your ip is not allowed to access the service."); \
        } \
        CHECK_ARGS("secret"); \
        if (api_secret != allArgs["secret"]) { \
            throw AuthException("Incorrect secret"); \
        } \
    } while(false);

#define CHECK_SECRET_OVERRIDE()                                                                                                                                \
    try {                                                                                                                                                      \
        CHECK_SECRET();                                                                                                                                        \
        enable_authorize = false;                                                                                                                              \
        break;                                                                                                                                                 \
    } catch (...) {}

#define CHECK_AUTH_TOKEN()                                                                                                                                     \
    GET_CONFIG(bool, _enable_authorize, Manager::kEnableAuthorize);                                                                                            \
    bool enable_authorize = _enable_authorize;                                                                                                                 \
    UserSessionCache::Ptr token_cache;                                                                                                                         \
    if (enable_authorize) {                                                                                                                                    \
        do {                                                                                                                                                   \
            /*CHECK_SECRET_OVERRIDE();*/                                                                                                                       \
            CHECK_ARGS_("Authorization");                                                                                                                      \
            string bearer_token = allArgs["Authorization"];                                                                                                    \
            string jwt_token = trim(findSubString(bearer_token.data(), "Bearer", nullptr));                                                                    \
            token_cache = UserAuthorManager::Instance().getTokenCache(jwt_token, allArgs["User-Agent"], sender.get_peer_ip());                                 \
            if (!token_cache->hasProjectAccess()) {                                                                                                            \
                throw AuthException("Unauthorized", ApiErrCode::CODE_UNAUTHORIZED);                                                                            \
            }                                                                                                                                                  \
            allArgs.args["_jwt_token"] = jwt_token;                                                                                                            \
            allArgs.args["_user_id"] = token_cache->getUid();                                                                                                  \
            allArgs.args["_user_name"] = token_cache->getUserName();                                                                                           \
            allArgs.args["_project_id"] = token_cache->getProjectId();                                                                                         \
        } while (false);                                                                                                                                       \
    }

bool checkUserAuthor(const std::string &resource_id, const std::string &jwt_token);

#define CHECK_USER_AUTHOR(key)                                                                                                                                 \
    CHECK_AUTH_TOKEN();                                                                                                                                        \
    do {                                                                                                                                                       \
        CHECK_ARGS_(key);                                                                                                                                      \
        string resource_id = allArgs[key];                                                                                                                     \
        string jwt_token = allArgs["_jwt_token"];                                                                                                              \
        if (!checkUserAuthor(resource_id, jwt_token)) {                                                                                                        \
            throw AuthException("No permission on key", ApiErrCode::CODE_PERMISSION_DENIED);                                                                   \
        }                                                                                                                                                      \
    } while (false);

#define CHECK_USER_DEVICE_AUTHOR_ASYNC(device_id, cb)                                                                                                          \
    string jwt_token = allArgs["_jwt_token"];                                                                                                                  \
    /* Note: do not throw error in this scope because it catch in do_http_hook scope */                                                                        \
    Broadcast::AuthInvoker auth_invoker = [allArgs, val, invoker, headerOut, cb](const string &err) mutable {                                                  \
        if (!err.empty()) {                                                                                                                                    \
            RETURN_API_RESPONSE(ApiErrCode::CODE_PERMISSION_DENIED, err.data());                                                                               \
            return;                                                                                                                                            \
        }                                                                                                                                                      \
        /* Authorized, execute the callback function */                                                                                                        \
        cb();                                                                                                                                                  \
    };                                                                                                                                                         \
    /* If authorization is not enabled, directly pass */                                                                                                       \
    if (!enable_authorize) {                                                                                                                                   \
        return auth_invoker("");                                                                                                                               \
    }                                                                                                                                                          \
    /* If there is no token, directly reject */                                                                                                                \
    if (jwt_token.empty()) {                                                                                                                                   \
        return auth_invoker("Empty token");                                                                                                                    \
    }                                                                                                                                                          \
    /* Broadcast to check device access authorization asynchronously */                                                                                        \
    auto flag = NOTICE_EMIT(BroadcastDeviceAccessArgs, Broadcast::kBroadcastDeviceAccess, device_id, jwt_token, auth_invoker);                                 \
    if (!flag) {                                                                                                                                               \
        /* No one is listening to the event, directly reject */                                                                                                \
        auth_invoker("Unauthorized");                                                                                                                          \
    }

#define RETURN_API_RESPONSE(code, msg)                                                                                                                         \
    std::string message(msg);                                                                                                                                  \
    if (message.empty()) {                                                                                                                                     \
        message = getDefaultMessage(static_cast<ApiErrCode>(code));                                                                                            \
    }                                                                                                                                                          \
    val["code"] = code;                                                                                                                                        \
    val["msg"] = message;                                                                                                                                      \
    auto status_code = getStatusCode(static_cast<ApiErrCode>(code));                                                                                           \
    invoker(status_code, headerOut, val.toStyledString());

bool checkPermissionCode(managerkit::UserSessionCache::Ptr &session, const std::string &key);

template<typename ...KeyTypes>
bool checkPermissionCode(managerkit::UserSessionCache::Ptr &session, const std::string &key, const KeyTypes &...keys) {
    return checkPermissionCode(session, key) && checkPermissionCode(session, keys...);
}

#define CHECK_USER_PERMISSION(...)                                                                                                                             \
    if (enable_authorize) {                                                                                                                                    \
        do {                                                                                                                                                   \
            string jwt_token = allArgs["_jwt_token"];                                                                                                          \
            auto token_cache = UserAuthorManager::Instance().getTokenCache(jwt_token);                                                                         \
            checkPermissionCode(token_cache, ##__VA_ARGS__);                                                                                                   \
        } while (false);                                                                                                                                       \
    }

#define CHECK_CLUSTER_AUTHOR_ASYNC(cb)                                                                                                                         \
    CHECK_ARGS_("secret");                                                                                                                                     \
    string authorId = allArgs["authorId"];                                                                                                                     \
    if (allArgs["authorId"].empty()) {                                                                                                                         \
        CHECK_SECRET();                                                                                                                                        \
        cb();                                                                                                                                                  \
        return;                                                                                                                                                \
    }                                                                                                                                                          \
    auto permit = UserAuthorManager::Instance().getClusterAuthorCache(allArgs["authorId"], allArgs["secret"]);                                                 \
    if (permit == UserAuthorPermit::REJECT) {                                                                                                                  \
        throw AuthException("Unauthorized", ApiErrCode::CODE_UNAUTHORIZED);                                                                                    \
    } else if (permit == UserAuthorPermit::ACCEPT) {                                                                                                           \
        cb();                                                                                                                                                  \
        return;                                                                                                                                                \
    }                                                                                                                                                          \
    Broadcast::AuthInvoker auth_invoker = [allArgs, val, invoker, headerOut, cb](const string &err) mutable {                                                  \
        /* Note: do not throw error in this scope because it catch in do_http_hook scope */                                                                    \
        UserAuthorManager::Instance().addClusterAuthorCache(allArgs["authorId"], allArgs["secret"], err.empty(), 600);                                         \
        if (!err.empty()) {                                                                                                                                    \
            RETURN_API_RESPONSE(ApiErrCode::CODE_UNAUTHORIZED, err.data());                                                                                    \
            return;                                                                                                                                            \
        }                                                                                                                                                      \
        /* Authorized, execute the callback function */                                                                                                        \
        cb();                                                                                                                                                  \
    };                                                                                                                                                         \
    /* Broadcast to check cluster across access authorization asynchronously */                                                                                \
    auto flag = NOTICE_EMIT(BroadcastClusterAcrossAccessArgs, Broadcast::kBroadcastClusterAcrossAccess, allArgs["authorId"], allArgs["secret"], auth_invoker); \
    if (!flag) {                                                                                                                                               \
        /* No one is listening to the event, directly reject */                                                                                                \
        auth_invoker("Unauthorized");                                                                                                                          \
    }

void installWebApi();
void unInstallWebApi();

#if defined(ENABLE_RTPPROXY)
uint16_t openRtpServer(uint16_t local_port, const mediakit::MediaTuple &tuple, int tcp_mode, const std::string &local_ip, bool re_use_port, uint32_t ssrc, int only_track, bool multiplex=false);
#endif

Json::Value makeMediaSourceJson(mediakit::MediaSource &media);
void getStatisticJson(const std::function<void(Json::Value &val)> &cb);
void addStreamProxy(const mediakit::MediaTuple &tuple, const std::string &url, int retry_count,
                    const mediakit::ProtocolOption &option, int rtp_type, float timeout_sec, const toolkit::mINI &args,
                    const std::function<void(const toolkit::SockException &ex, const std::string &key)> &cb);
void addStreamProxy(const mediakit::MediaTuple &tuple, const mediakit::ProtocolOption &option,
                    const std::function<void(const std::string &err, const mediakit::PlayerProxy::Ptr &ptr)> &cb);
void delStreamProxy(const mediakit::MediaTuple &tuple);
void addReplayStreamProxy(const mediakit::MediaTuple &tuple, const mediakit::ProtocolOption &option,
                    const std::function<void(const std::string &err, const mediakit::PlayerProxy::Ptr &ptr)> &cb, int retry_count);
const mediakit::PlayerProxy::Ptr getReplayStreamProxy(const std::string &key);
void delReplayStreamProxy(const std::string &key);
void addFFmpegSource(const std::string &dst_url, const std::function<void(const std::string &err, const FFmpegSource::Ptr &player)> &cb);
void delFFmpegSource(const std::string &dst_url);

template <typename Type>
class ServiceController {
public:
    using Pointer = std::shared_ptr<Type>;
    std::unordered_map<std::string, Pointer> _map;
    mutable std::recursive_mutex _mtx;

    void clear() {
        decltype(_map) copy;
        {
            std::lock_guard<std::recursive_mutex> lck(_mtx);
            copy.swap(_map);
        }
    }

    size_t erase(const std::string &key) {
        Pointer erase_ptr;
        {
            std::lock_guard<std::recursive_mutex> lck(_mtx);
            auto itr = _map.find(key);
            if (itr != _map.end()) {
                erase_ptr = std::move(itr->second);
                _map.erase(itr);
                return 1;
            }
        }
        return 0;
    }

    size_t size() { 
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        return _map.size();
    }

    Pointer find(const std::string &key) const {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        auto it = _map.find(key);
        if (it == _map.end()) {
            return nullptr;
        }
        return it->second;
    }

    void for_each(const std::function<void(const std::string&, const Pointer&)>& cb) {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        auto it = _map.begin();
        while (it != _map.end()) {
            cb(it->first, it->second);
            it++;
        }
    }

    template<class ..._Args>
    Pointer make(const std::string &key, _Args&& ...__args) {
        // assert(!find(key));

        auto server = std::make_shared<Type>(std::forward<_Args>(__args)...);
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        auto it = _map.emplace(key, server);
        assert(it.second);
        return server;
    }

    template<class ..._Args>
    Pointer makeWithAction(const std::string &key, std::function<void(Pointer)> action, _Args&& ...__args) {
        // assert(!find(key));

        auto server = std::make_shared<Type>(std::forward<_Args>(__args)...);
        action(server);
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        auto it = _map.emplace(key, server);
        assert(it.second);
        return server;
    }

    template<class ..._Args>
    Pointer emplace(const std::string &key, _Args&& ...__args) {
        // assert(!find(key));

        auto server = std::static_pointer_cast<Type>(std::forward<_Args>(__args)...);
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        auto it = _map.emplace(key, server);
        assert(it.second);
        return server;
    }
};

#if defined(ENABLE_WEBRTC)
template <typename Args>
class WebRtcArgsImp : public mediakit::WebRtcArgs {
public:
    WebRtcArgsImp(const HttpAllArgs<Args> &args, std::string session_id)
        : _args(args)
        , _session_id(std::move(session_id)) {}
    ~WebRtcArgsImp() override = default;

    toolkit::variant operator[](const std::string &key) const override {
        if (key == "url") {
            return getUrl();
        }
        return _args[key];
    }

private:
    std::string getUrl() const {
        auto &allArgs = _args;
        CHECK_ARGS("app", "stream");

        return StrPrinter << RTC_SCHEMA << "://" << (_args["Host"].empty() ? DEFAULT_VHOST : _args["Host"].data()) << "/" << _args["app"] << "/"
                          << _args["stream"] << "?" << _args.getParser().params() + "&session=" + _session_id;
    }

private:
    HttpAllArgs<Args> _args;
    std::string _session_id;
};
#endif // defined(ENABLE_WEBRTC)

namespace managerkit {
// Register the Web API configuration endpoints
void registerConfigurationApis();

// Register the Web API monitor endpoints
void registerMonitorApis();

// Register the Web API extraction endpoints
void registerExtractionApis();
void unregisterExtractionApis();

// Register the Web API motion endpoints
void registerMotionDetectionApis();

// Register the Web API playback endpoints
void registerPlaybackApis();

// Register the Web API bookmark endpoints
void registerBookmarkApis();

// Register the Web API sync database endpoints
void registerSyncDbApis();

// Register the Web API control endpoints
void registerControlApis();
void unregisterControlApis();

// Register the Web API storage endpoints 
void registerStorageApis();

// Register the Web API SD card sync endpoints 
void registerHistoricalSDCardSyncApis();

} // namespace managerkit

#endif //S3MEDIAKIT_WEBAPI_H
