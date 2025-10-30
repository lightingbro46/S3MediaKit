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
    AuthException(const char *str) : ApiRetException(str, API::AuthFailed, 401) {}
};

class InvalidArgsException: public ApiRetException {
public:
    InvalidArgsException(const char *str) : ApiRetException(str, API::InvalidArgs, 400) {}
};

class SuccessException: public ApiRetException {
public:
    SuccessException() : ApiRetException("success", API::Success, 200) {}
};

using ApiArgsType = std::map<std::string, std::string, mediakit::StrCaseCompare>;

template<typename Args, typename First>
std::string getValue(Args &args, const First &first) {
    return args[first];
}

template<typename First>
std::string getValue(Json::Value &args, const First &first) {
    return args[first].asString();
}

template<typename First>
std::string getValue(std::string &args, const First &first) {
    return "";
}

template<typename First>
std::string getValue(const mediakit::Parser &parser, const First &first) {
    auto ret = parser.getUrlArgs()[first];
    if (!ret.empty()) {
        return ret;
    }
    return parser.getHeader()[first];
}

template<typename First>
std::string getValue(mediakit::Parser &parser, const First &first) {
    return getValue((const mediakit::Parser &) parser, first);
}

template<typename Args, typename First>
std::string getValue(const mediakit::Parser &parser, Args &args, const First &first) {
    auto ret = getValue(args, first);
    if (!ret.empty()) {
        return ret;
    }
    return getValue(parser, first);
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

template<typename Args, typename First>
bool checkArgs(Args &args, const First &first) {
    return !args[first].empty();
}

template<typename Args, typename First, typename ...KeyTypes>
bool checkArgs(Args &args, const First &first, const KeyTypes &...keys) {
    return checkArgs(args, first) && checkArgs(args, keys...);
}

// Check whether the http url, body or http header parameters are empty
#define CHECK_ARGS(...)  \
    if(!checkArgs(allArgs,##__VA_ARGS__)){ \
        throw InvalidArgsException("Required parameter missed: " #__VA_ARGS__); \
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

#define CHECK_AUTH_TOKEN()                                                                                                                                     \
    CHECK_ARGS("Authorization");                                                                                                                               \
    string bearer_token = allArgs["Authorization"];                                                                                                            \
    string jwt_token = trim(findSubString(bearer_token.data(), "Bearer", nullptr));                                                                            \
    auto token_cache = UserAuthorManager::Instance().getTokenCache(jwt_token);                                                                                 \
    GET_CONFIG(bool, enable_authorize, Manager::kEnableAuthorize);                                                                                             \
    if (!token_cache->hasAccess() && enable_authorize) {                                                                                                       \
        throw AuthException("Unauthorized");                                                                                                                   \
    }                                                                                                                                                          \
    allArgs.args["_user_id"] = token_cache->getUid();                                                                                                          \
    allArgs.args["_user_name"] = token_cache->getUserName();                                                                                                   \
    allArgs.args["_project_id"] = token_cache->getProjectId();

#define CHECK_USER_AUTHOR(resource_id)                                                                                                                         \
    if (!checkUserAuthor(resource_id, jwt_token)) {                                                                                                            \
        throw AuthException("Unauthorized");                                                                                                                   \
    }

#define CHECK_USER_DEVICE_AUTHOR_ASYNC(device_id, cb) checkUserDeviceAuthor((device_id), jwt_token, (cb));

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
void addFFmpegSource(const std::string &dst_url, const std::function<void(const std::string &err, const FFmpegSource::Ptr &player)> &cb);
void delFFmpegSource(const std::string &dst_url);

#endif //S3MEDIAKIT_WEBAPI_H
